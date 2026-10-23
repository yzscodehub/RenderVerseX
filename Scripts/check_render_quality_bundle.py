#!/usr/bin/env python3
"""Fail-closed validation for a RenderQualityLab semantic attachment bundle.

The renderer writes a versioned JSON manifest next to raw little-endian
float32 attachments.  This tool deliberately validates the capture boundary
before comparing any pixels: a screenshot with the right colours is not
qualification evidence when its source commit, execution receipt, frame, or
attachment payload cannot be proven.

The container is intentionally compact and dependency-free. Version 2 uses
only raw float32 payloads so HDR, depth, normals, velocity, AO, and masks keep
their original values; PPM is emitted only as a human-readable diff heatmap.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import re
import struct
import sys
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
from typing import Any, Iterable


SCHEMA = "RVX.RenderQualityAttachmentBundle"
SCHEMA_VERSION = 2
POLICY_SCHEMA = "RVX.RenderQualityVisualPolicy"
POLICY_VERSION = 2
SHA256 = re.compile(r"^[0-9a-f]{64}$")
REQUIRED_SEMANTICS = (
    "BeautyLDR",
    "PreToneHDR",
    "LinearDepth",
    "Normal",
    "Velocity",
    "AmbientOcclusion",
    "ReactiveMask",
)
SEMANTIC_DOMAINS = {
    "BeautyLDR": "LDR",
    "PreToneHDR": "HDR",
    "LinearDepth": "LinearDepth",
    "Normal": "WorldNormal",
    "Velocity": "Motion",
    "AmbientOcclusion": "LinearMask",
    "ReactiveMask": "LinearMask",
}
SEMANTIC_FORMATS = {
    "BeautyLDR": "RGBA32_FLOAT",
    "PreToneHDR": "RGBA32_FLOAT",
    "LinearDepth": "R32_FLOAT",
    "Normal": "RGBA32_FLOAT",
    "Velocity": "RG32_FLOAT",
    "AmbientOcclusion": "R32_FLOAT",
    "ReactiveMask": "R32_FLOAT",
}
VIEW_ROLES = {"Main", "Shadow", "Reflection", "Editor"}
CONFIGURATION_FIELDS = {
    "renderPath",
    "instancingMode",
    "requestedRecipe",
    "resolvedRecipe",
    "profileHash",
    "renderConfigurationHash",
}
RENDER_PATHS = {"auto", "direct", "gpu-driven"}
INSTANCING_MODES = {"Disabled", "Auto"}
PRESENTABLE_RECIPE_IDS = {
    "forward-plus": 1,
    "deferred": 2,
    "hybrid-rt": 3,
}
QUALITY_IDS = {
    "low": 0,
    "medium": 1,
    "high": 2,
    "ultra": 3,
}
BACKENDS = {"dx11", "dx12", "vulkan", "metal", "opengl"}

# Qualification captures are deliberately bounded before the validator opens a
# payload.  These limits cover the 1280x720 qualification output with ample
# room for a future 4K capture, while making a malformed manifest unable to
# turn an offline validator into an unbounded memory or disk reader.
MAX_ATTACHMENT_WIDTH = 8192
MAX_ATTACHMENT_HEIGHT = 8192
MAX_ATTACHMENT_PIXELS = 64 * 1024 * 1024
MAX_ATTACHMENT_PAYLOAD_BYTES = 256 * 1024 * 1024
MAX_BUNDLE_PAYLOAD_BYTES = 512 * 1024 * 1024


class ContractError(ValueError):
    """Input cannot be used as fail-closed qualification evidence."""


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
        raise ContractError(f"{context} is missing required fields: {sorted(missing)}")
    if unknown:
        raise ContractError(f"{context} contains unsupported fields: {sorted(unknown)}")


def _string(value: Any, context: str) -> str:
    if not isinstance(value, str) or not value:
        raise ContractError(f"{context} must be a non-empty string")
    return value


def _integer(value: Any, context: str, minimum: int = 0) -> int:
    if type(value) is not int or value < minimum:
        raise ContractError(f"{context} must be an integer >= {minimum}")
    return value


def _number(value: Any, context: str, minimum: float | None = None) -> float:
    if type(value) not in (int, float) or not math.isfinite(float(value)):
        raise ContractError(f"{context} must be a finite number")
    result = float(value)
    if minimum is not None and result < minimum:
        raise ContractError(f"{context} must be >= {minimum}")
    return result


def _sha256(value: Any, context: str) -> str:
    result = _string(value, context)
    if not SHA256.fullmatch(result):
        raise ContractError(f"{context} must be a lowercase SHA-256 digest")
    return result


def _safe_relative(value: Any, context: str) -> PurePosixPath:
    text = _string(value, context)
    path = PurePosixPath(text)
    if path.is_absolute() or ".." in path.parts or not path.parts:
        raise ContractError(f"{context} must be a package-relative path")
    if "\\" in text or ":" in text:
        raise ContractError(f"{context} must not contain a drive or backslash")
    return path


def _resolve(root: Path, value: Any, context: str) -> Path:
    relative = _safe_relative(value, context)
    candidate = (root / Path(*relative.parts)).resolve()
    try:
        candidate.relative_to(root.resolve())
    except ValueError as error:
        raise ContractError(f"{context} escapes the bundle directory") from error
    return candidate


def _hash(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while data := stream.read(1024 * 1024):
            digest.update(data)
    return digest.hexdigest()


def _preflight_attachment_payload_budget(attachments: Any) -> None:
    """Reject impossible payload declarations before opening any raw file."""

    if not isinstance(attachments, list):
        return
    aggregate = 0
    for index, attachment in enumerate(attachments):
        if not isinstance(attachment, dict):
            continue
        # Let the detailed parser report malformed schemas.  This preflight is
        # solely an I/O budget and therefore considers only well-typed size
        # declarations it can prove to be dangerous.
        width = attachment.get("width")
        height = attachment.get("height")
        payload_bytes = attachment.get("payloadBytes")
        if type(width) is not int or type(height) is not int or type(payload_bytes) is not int:
            continue
        if width < 1 or height < 1 or payload_bytes < 1:
            continue
        if width > MAX_ATTACHMENT_WIDTH or height > MAX_ATTACHMENT_HEIGHT or width * height > MAX_ATTACHMENT_PIXELS:
            raise ContractError(f"attachments[{index}] extent exceeds qualification capture limits")
        if payload_bytes > MAX_ATTACHMENT_PAYLOAD_BYTES:
            raise ContractError(f"attachments[{index}] payload exceeds qualification capture limits")
        aggregate += payload_bytes
        if aggregate > MAX_BUNDLE_PAYLOAD_BYTES:
            raise ContractError("attachment bundle payload aggregate exceeds qualification capture limits")


@dataclass(frozen=True)
class Attachment:
    semantic: str
    path: Path
    format: str
    color_domain: str
    width: int
    height: int
    channels: int
    row_pitch: int
    payload_bytes: int
    digest: str
    provenance: dict[str, Any]
    values: tuple[float, ...]


@dataclass(frozen=True)
class Bundle:
    path: Path
    bundle_id: str
    source_commit: str
    qualification_identity: dict[str, str | int]
    render_configuration_identity: dict[str, str | int]
    execution: dict[str, str | int | bool]
    attachments: dict[str, Attachment]


def _configuration_hash(
    configuration: dict[str, str | int],
    identity: dict[str, str | int],
) -> str:
    binding = bytearray()
    for name, item in (
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
        encoded = str(item).encode("utf-8")
        binding.extend(name.encode("ascii"))
        binding.extend(b":")
        binding.extend(str(len(encoded)).encode("ascii"))
        binding.extend(b":")
        binding.extend(encoded)
        binding.extend(b"\n")
    return hashlib.sha256(binding).hexdigest()


def _profile_hash(recipe: str, quality: str) -> int:
    try:
        values = (1, PRESENTABLE_RECIPE_IDS[recipe], QUALITY_IDS[quality])
    except KeyError as error:
        raise ContractError(
            "render configuration recipe or quality is unsupported"
        ) from error
    result = 14695981039346656037
    for value in values:
        for byte in range(8):
            result ^= (value >> (byte * 8)) & 0xFF
            result = (result * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return result or 1


def _render_configuration(
    value: Any,
    identity: dict[str, str | int],
) -> dict[str, str | int]:
    if not isinstance(value, dict):
        raise ContractError("renderConfigurationIdentity must be an object")
    _require_keys(
        value,
        CONFIGURATION_FIELDS,
        "renderConfigurationIdentity",
    )
    result: dict[str, str | int] = {
        key: _string(value[key], f"renderConfigurationIdentity.{key}")
        for key in (
            "renderPath",
            "instancingMode",
            "requestedRecipe",
            "resolvedRecipe",
        )
    }
    if result["renderPath"] not in RENDER_PATHS:
        raise ContractError("renderConfigurationIdentity.renderPath is unsupported")
    if result["instancingMode"] not in INSTANCING_MODES:
        raise ContractError(
            "renderConfigurationIdentity.instancingMode is unsupported"
        )
    if result["requestedRecipe"] not in PRESENTABLE_RECIPE_IDS:
        raise ContractError(
            "renderConfigurationIdentity.requestedRecipe is unsupported"
        )
    if result["resolvedRecipe"] not in PRESENTABLE_RECIPE_IDS:
        raise ContractError(
            "renderConfigurationIdentity.resolvedRecipe is unsupported"
        )
    if identity["pipeline"] not in PRESENTABLE_RECIPE_IDS:
        raise ContractError("qualificationIdentity.pipeline is unsupported")
    if identity["quality"] not in QUALITY_IDS:
        raise ContractError("qualificationIdentity.quality is unsupported")
    if identity["backend"] not in BACKENDS:
        raise ContractError("qualificationIdentity.backend is unsupported")
    if result["resolvedRecipe"] != identity["pipeline"]:
        raise ContractError(
            "renderConfigurationIdentity.resolvedRecipe does not match "
            "qualificationIdentity.pipeline"
        )
    result["profileHash"] = _integer(
        value["profileHash"], "renderConfigurationIdentity.profileHash", 1
    )
    if result["profileHash"] > (2**64 - 1):
        raise ContractError("renderConfigurationIdentity.profileHash exceeds uint64")
    expected_profile_hash = _profile_hash(
        str(result["resolvedRecipe"]), str(identity["quality"])
    )
    if result["profileHash"] != expected_profile_hash:
        raise ContractError(
            "renderConfigurationIdentity.profileHash does not match "
            "the resolved recipe and quality"
        )
    result["renderConfigurationHash"] = _sha256(
        value["renderConfigurationHash"],
        "renderConfigurationIdentity.renderConfigurationHash",
    )
    if result["renderConfigurationHash"] != _configuration_hash(result, identity):
        raise ContractError(
            "renderConfigurationIdentity.renderConfigurationHash does not bind runtime configuration"
        )
    return result


def _read_attachment(
    root: Path,
    value: Any,
    source_commit: str,
    execution: dict[str, str | int | bool],
    index: int,
) -> Attachment:
    if not isinstance(value, dict):
        raise ContractError(f"attachments[{index}] must be an object")
    required = {
        "semantic", "dataPath", "format", "colorDomain", "width", "height", "channels",
        "scalarType", "rowPitchBytes", "payloadBytes", "sha256", "provenance",
    }
    _require_keys(value, required, f"attachments[{index}]")
    semantic = _string(value["semantic"], f"attachments[{index}].semantic")
    if semantic not in SEMANTIC_DOMAINS:
        raise ContractError(f"attachments[{index}].semantic is unsupported: {semantic}")
    format_name = _string(value["format"], f"attachments[{index}].format")
    if format_name not in {"R32_FLOAT", "RG32_FLOAT", "RGBA32_FLOAT"}:
        raise ContractError(f"attachments[{index}].format is unsupported: {format_name}")
    if format_name != SEMANTIC_FORMATS[semantic]:
        raise ContractError(f"attachments[{index}].format must be {SEMANTIC_FORMATS[semantic]} for {semantic}")
    color_domain = _string(value["colorDomain"], f"attachments[{index}].colorDomain")
    if color_domain != SEMANTIC_DOMAINS[semantic]:
        raise ContractError(
            f"attachments[{index}].colorDomain must be {SEMANTIC_DOMAINS[semantic]} for {semantic}"
        )
    width = _integer(value["width"], f"attachments[{index}].width", 1)
    height = _integer(value["height"], f"attachments[{index}].height", 1)
    if width > MAX_ATTACHMENT_WIDTH or height > MAX_ATTACHMENT_HEIGHT:
        raise ContractError(f"attachments[{index}] extent exceeds qualification capture limits")
    if width * height > MAX_ATTACHMENT_PIXELS:
        raise ContractError(f"attachments[{index}] pixel count exceeds qualification capture limits")
    channels = _integer(value["channels"], f"attachments[{index}].channels", 1)
    expected_channels = {"R32_FLOAT": 1, "RG32_FLOAT": 2, "RGBA32_FLOAT": 4}[format_name]
    if channels != expected_channels:
        raise ContractError(f"attachments[{index}].channels does not match {format_name}")
    if value["scalarType"] != "float32le":
        raise ContractError(f"attachments[{index}].scalarType must be float32le")
    tight_row_pitch = width * channels * 4
    row_pitch = _integer(value["rowPitchBytes"], f"attachments[{index}].rowPitchBytes", tight_row_pitch)
    if row_pitch != tight_row_pitch:
        raise ContractError(
            f"attachments[{index}].rowPitchBytes must be the canonical tight float32 row pitch"
        )
    payload_bytes = _integer(value["payloadBytes"], f"attachments[{index}].payloadBytes", 1)
    if payload_bytes != row_pitch * height:
        raise ContractError(f"attachments[{index}].payloadBytes must equal rowPitchBytes * height")
    if payload_bytes > MAX_ATTACHMENT_PAYLOAD_BYTES:
        raise ContractError(f"attachments[{index}] payload exceeds qualification capture limits")
    digest = _sha256(value["sha256"], f"attachments[{index}].sha256")
    if not isinstance(value["provenance"], dict):
        raise ContractError(f"attachments[{index}].provenance must be an object")
    provenance = value["provenance"]
    _require_keys(
        provenance,
        {
            "sourceCommit", "runId", "preset", "renderExecutionReceiptSha256",
            "frameSequence", "submittedFrame", "completedFrame", "sceneRevision",
            "surfaceGeneration", "configurationRevision", "viewId", "viewSceneRuntimeId",
            "viewCameraEntityPacked", "viewSubViewIndex", "viewRole", "graphIdentity",
            "graphRecordingGeneration", "graphRecordEpoch", "graphResourceIdentity",
            "graphResourceGeneration", "producerIdentity", "resourceName", "graphPasses",
        },
        f"attachments[{index}].provenance",
    )
    if _string(provenance["sourceCommit"], "attachment provenance sourceCommit") != source_commit:
        raise ContractError(f"attachments[{index}] source commit provenance does not match bundle")
    if _string(provenance["runId"], "attachment provenance runId") != execution["runId"]:
        raise ContractError(f"attachments[{index}] run provenance does not match bundle execution")
    if _string(provenance["preset"], "attachment provenance preset") != execution["preset"]:
        raise ContractError(f"attachments[{index}] preset provenance does not match bundle execution")
    if _sha256(provenance["renderExecutionReceiptSha256"], "attachment provenance receipt") != execution["renderExecutionReceiptSha256"]:
        raise ContractError(f"attachments[{index}] receipt provenance does not match bundle execution receipt")
    for field in (
        "frameSequence", "submittedFrame", "completedFrame", "sceneRevision",
        "surfaceGeneration", "configurationRevision",
    ):
        if _integer(provenance[field], f"attachment provenance {field}", 0) != execution[field]:
            raise ContractError(f"attachments[{index}] {field} provenance does not match bundle execution")
    for field in ("viewId", "viewRole"):
        if _string(provenance[field], f"attachment provenance {field}") != execution[field]:
            raise ContractError(f"attachments[{index}] {field} provenance does not match bundle execution")
    if provenance["viewRole"] not in VIEW_ROLES:
        raise ContractError("attachment provenance viewRole is unsupported")
    for field in ("viewSceneRuntimeId", "viewCameraEntityPacked", "viewSubViewIndex"):
        if _integer(provenance[field], f"attachment provenance {field}", 0) != execution[field]:
            raise ContractError(f"attachments[{index}] {field} provenance does not match bundle execution")
    _string(provenance["resourceName"], "attachment provenance resource")
    for field in (
        "graphIdentity", "graphRecordingGeneration", "graphRecordEpoch",
        "graphResourceIdentity", "graphResourceGeneration", "producerIdentity",
    ):
        _integer(provenance[field], f"attachment provenance {field}", 1)
    for field in ("graphIdentity", "graphRecordingGeneration", "graphRecordEpoch"):
        if _integer(provenance[field], f"attachment provenance {field}", 1) != execution[field]:
            raise ContractError(f"attachments[{index}] {field} provenance does not match bundle execution")
    passes = provenance["graphPasses"]
    if not isinstance(passes, list) or not passes or any(not isinstance(item, str) or not item for item in passes):
        raise ContractError("attachment provenance graphPasses must be a non-empty string list")
    path = _resolve(root, value["dataPath"], f"attachments[{index}].dataPath")
    try:
        payload = path.read_bytes()
    except OSError as error:
        raise ContractError(f"attachments[{index}] payload is missing: {error}") from error
    if len(payload) != payload_bytes:
        raise ContractError(f"attachments[{index}] payload is truncated or has trailing data")
    if _hash(path) != digest:
        raise ContractError(f"attachments[{index}] payload SHA-256 does not match metadata")

    values: list[float] = []
    used_bytes = width * channels * 4
    for row in range(height):
        row_start = row * row_pitch
        for pixel in struct.iter_unpack("<f", payload[row_start:row_start + used_bytes]):
            if not math.isfinite(pixel[0]):
                raise ContractError(f"attachments[{index}] contains NaN or infinity")
            values.append(pixel[0])
    return Attachment(
        semantic=semantic, path=path, format=format_name, color_domain=color_domain,
        width=width, height=height, channels=channels, row_pitch=row_pitch,
        payload_bytes=payload_bytes, digest=digest, provenance=provenance,
        values=tuple(values),
    )


def load_bundle(path: Path) -> Bundle:
    document = _load_json(path, "attachment bundle")
    _require_keys(
        document,
        {"schema", "schemaVersion", "bundleId", "sourceCommit", "qualificationIdentity", "renderConfigurationIdentity", "execution", "attachments"},
        "attachment bundle",
    )
    if document["schema"] != SCHEMA or document["schemaVersion"] != SCHEMA_VERSION:
        raise ContractError("attachment bundle schema/version is unsupported")
    bundle_id = _string(document["bundleId"], "bundleId")
    source_commit = _string(document["sourceCommit"], "sourceCommit")
    qualification_identity = document["qualificationIdentity"]
    if not isinstance(qualification_identity, dict):
        raise ContractError("qualificationIdentity must be an object")
    _require_keys(
        qualification_identity,
        {"sampleId", "pipeline", "quality", "backend", "view", "captureTick", "visualContractVersion"},
        "qualificationIdentity",
    )
    identity: dict[str, str | int] = {}
    for key in ("sampleId", "pipeline", "quality", "backend", "view"):
        identity[key] = _string(qualification_identity[key], f"qualificationIdentity.{key}")
    for key in ("captureTick", "visualContractVersion"):
        identity[key] = _integer(qualification_identity[key], f"qualificationIdentity.{key}", 0)
    render_configuration_identity = _render_configuration(
        document["renderConfigurationIdentity"],
        identity,
    )

    execution_document = document["execution"]
    if not isinstance(execution_document, dict):
        raise ContractError("execution must be an object")
    _require_keys(
        execution_document,
        {
            "runId", "preset", "renderExecutionReceiptSha256", "completionOwned",
            "frameSequence", "submittedFrame", "completedFrame", "sceneRevision",
            "surfaceGeneration", "configurationRevision", "viewId", "viewSceneRuntimeId",
            "viewCameraEntityPacked", "viewSubViewIndex", "viewRole", "graphIdentity",
            "graphRecordingGeneration", "graphRecordEpoch",
        },
        "execution",
    )
    execution: dict[str, str | int | bool] = {
        "runId": _string(execution_document["runId"], "execution.runId"),
        "preset": _string(execution_document["preset"], "execution.preset"),
        "renderExecutionReceiptSha256": _sha256(execution_document["renderExecutionReceiptSha256"], "execution.renderExecutionReceiptSha256"),
        "completionOwned": execution_document["completionOwned"],
        "viewId": _string(execution_document["viewId"], "execution.viewId"),
        "viewRole": _string(execution_document["viewRole"], "execution.viewRole"),
    }
    if execution["completionOwned"] is not True:
        raise ContractError("execution.completionOwned must be true")
    if execution["viewRole"] not in VIEW_ROLES:
        raise ContractError("execution.viewRole is unsupported")
    execution["frameSequence"] = _integer(
        execution_document["frameSequence"], "execution.frameSequence", 1
    )
    execution["submittedFrame"] = _integer(
        execution_document["submittedFrame"], "execution.submittedFrame", 1
    )
    execution["completedFrame"] = _integer(
        execution_document["completedFrame"], "execution.completedFrame", 1
    )
    for key in (
        "sceneRevision", "surfaceGeneration", "configurationRevision", "viewSceneRuntimeId",
        "viewCameraEntityPacked", "viewSubViewIndex", "graphIdentity",
        "graphRecordingGeneration", "graphRecordEpoch",
    ):
        minimum = 0 if key == "viewSubViewIndex" else 1
        execution[key] = _integer(execution_document[key], f"execution.{key}", minimum)
    if execution["completedFrame"] < execution["submittedFrame"]:
        raise ContractError("execution.completedFrame precedes execution.submittedFrame")

    attachment_documents = document["attachments"]
    if not isinstance(attachment_documents, list) or len(attachment_documents) != len(REQUIRED_SEMANTICS):
        raise ContractError(f"attachments must contain exactly {len(REQUIRED_SEMANTICS)} semantic outputs")
    _preflight_attachment_payload_budget(attachment_documents)
    attachments: dict[str, Attachment] = {}
    observed_resources: set[tuple[int, int]] = set()
    for index, item in enumerate(attachment_documents):
        attachment = _read_attachment(
            path.parent,
            item,
            source_commit,
            execution,
            index,
        )
        if attachment.semantic in attachments:
            raise ContractError(f"attachment semantic is duplicated: {attachment.semantic}")
        resource_key = (
            int(attachment.provenance["graphResourceIdentity"]),
            int(attachment.provenance["graphResourceGeneration"]),
        )
        if resource_key in observed_resources:
            raise ContractError("attachment graph resource provenance is duplicated")
        observed_resources.add(resource_key)
        attachments[attachment.semantic] = attachment
    if tuple(sorted(attachments)) != tuple(sorted(REQUIRED_SEMANTICS)):
        raise ContractError("attachment semantic set is incomplete")
    return Bundle(
        path,
        bundle_id,
        source_commit,
        identity,
        render_configuration_identity,
        execution,
        attachments,
    )


def load_policy(path: Path) -> dict[str, Any]:
    document = _load_json(path, "visual policy")
    _require_keys(document, {"schema", "schemaVersion", "qualificationIdentity", "attachments", "temporal"}, "visual policy")
    if document["schema"] != POLICY_SCHEMA or document["schemaVersion"] != POLICY_VERSION:
        raise ContractError("visual policy schema/version is unsupported")
    identity = document["qualificationIdentity"]
    if not isinstance(identity, dict):
        raise ContractError("visual policy qualificationIdentity must be an object")
    expected_identity_keys = {
        "sampleId", "pipeline", "quality", "backend", "view",
        "visualContractVersion",
    }
    _require_keys(identity, expected_identity_keys, "visual policy qualificationIdentity")
    normalized_identity: dict[str, str | int] = {}
    for key in ("sampleId", "pipeline", "quality", "backend", "view"):
        normalized_identity[key] = _string(identity[key], f"visual policy qualificationIdentity.{key}")
    normalized_identity["visualContractVersion"] = _integer(
        identity["visualContractVersion"],
        "visual policy qualificationIdentity.visualContractVersion",
        1,
    )
    attachment_policies = document["attachments"]
    if not isinstance(attachment_policies, dict) or set(attachment_policies) != set(REQUIRED_SEMANTICS):
        raise ContractError("visual policy must provide one ROI policy for every semantic attachment")
    for semantic, item in attachment_policies.items():
        if not isinstance(item, dict):
            raise ContractError(f"visual policy attachment {semantic} must be an object")
        _require_keys(item, {"rois"}, f"visual policy attachment {semantic}")
        rois = item["rois"]
        if not isinstance(rois, list) or not rois:
            raise ContractError(f"visual policy attachment {semantic} requires at least one ROI")
        seen_roi_ids: set[str] = set()
        for index, roi in enumerate(rois):
            if not isinstance(roi, dict):
                raise ContractError(f"visual policy {semantic}.rois[{index}] must be an object")
            _require_keys(
                roi,
                {
                    "id", "normalizedRegionId", "x", "y", "width", "height",
                    "temporalMode", "dataRange", "maxMae", "maxAbs", "minPsnr",
                    "minSsim", "minTemporalMae", "maxTemporalMae",
                },
                f"visual policy {semantic}.rois[{index}]",
            )
            roi_id = _string(roi["id"], f"visual policy {semantic}.rois[{index}].id")
            if roi_id in seen_roi_ids:
                raise ContractError(f"visual policy {semantic} has duplicate ROI id {roi_id}")
            seen_roi_ids.add(roi_id)
            _string(
                roi["normalizedRegionId"],
                f"visual policy {semantic}.rois[{index}].normalizedRegionId",
            )
            for key in ("x", "y"):
                _integer(roi[key], f"visual policy {semantic}.rois[{index}].{key}", 0)
            for key in ("width", "height"):
                _integer(roi[key], f"visual policy {semantic}.rois[{index}].{key}", 1)
            if roi["temporalMode"] not in {"static", "dynamic"}:
                raise ContractError(f"visual policy {semantic}.rois[{index}].temporalMode must be static or dynamic")
            if _number(
                roi["dataRange"],
                f"visual policy {semantic}.rois[{index}].dataRange",
                0.0,
            ) <= 0.0:
                raise ContractError("ROI dataRange must be greater than zero")
            for key in (
                "maxMae", "maxAbs", "minPsnr", "minSsim",
                "minTemporalMae", "maxTemporalMae",
            ):
                _number(roi[key], f"visual policy {semantic}.rois[{index}].{key}", 0.0)
            if _number(roi["minSsim"], "ROI minSsim") > 1.0:
                raise ContractError("ROI minSsim cannot exceed 1")
            if float(roi["minTemporalMae"]) > float(roi["maxTemporalMae"]):
                raise ContractError("ROI minTemporalMae cannot exceed maxTemporalMae")
            if (
                roi["temporalMode"] == "static"
                and float(roi["minTemporalMae"]) != 0.0
            ):
                raise ContractError("static ROI minTemporalMae must be zero")
    temporal = document["temporal"]
    if not isinstance(temporal, dict):
        raise ContractError("visual policy temporal must be an object")
    _require_keys(
        temporal,
        {"warmupCompletedFrames", "goldenCaptureTick", "adjacentCaptureTicks"},
        "visual policy temporal",
    )
    warmup_completed_frames = _integer(
        temporal["warmupCompletedFrames"],
        "temporal.warmupCompletedFrames",
        60,
    )
    if warmup_completed_frames < 60:
        raise ContractError("temporal.warmupCompletedFrames must be at least 60")
    golden_tick = _integer(
        temporal["goldenCaptureTick"], "temporal.goldenCaptureTick", 1
    )
    if golden_tick < warmup_completed_frames:
        raise ContractError(
            "temporal.goldenCaptureTick must not precede "
            "temporal.warmupCompletedFrames"
        )
    adjacent_ticks = temporal["adjacentCaptureTicks"]
    if (
        not isinstance(adjacent_ticks, list)
        or len(adjacent_ticks) != 2
        or any(type(value) is not int or value <= 0 for value in adjacent_ticks)
        or adjacent_ticks[1] != adjacent_ticks[0] + 1
        or golden_tick >= adjacent_ticks[0]
    ):
        raise ContractError(
            "temporal adjacentCaptureTicks must be one ordered adjacent pair after the Golden tick"
        )
    document["qualificationIdentity"] = normalized_identity
    return document


def _unit_interval(value: Any, context: str) -> float:
    result = _number(value, context, 0.0)
    if result > 1.0:
        raise ContractError(f"{context} must be <= 1")
    return result


def load_sample_contract(
    path: Path,
    sample_id: str,
) -> tuple[dict[str, tuple[float, float, float, float]], tuple[int, ...]]:
    """Load public normalized ROIs without consulting renderer-private state.

    The sample manifest is the source of truth for a scenario's semantic
    regions.  The visual policy carries only the deterministic integer result
    for each attachment extent; this prevents individual semantic policies
    from drifting from the scenario's reviewable normalized contract.
    """

    document = _load_json(path, "sample manifest")
    _require_keys(document, {"schema", "schemaVersion", "samples"}, "sample manifest")
    if document["schema"] != "RVX.SampleManifest" or document["schemaVersion"] != 1:
        raise ContractError("sample manifest schema/version is unsupported")
    samples = document["samples"]
    if not isinstance(samples, list):
        raise ContractError("sample manifest samples must be an array")
    matched = [item for item in samples if isinstance(item, dict) and item.get("id") == sample_id]
    if len(matched) != 1:
        raise ContractError(f"sample manifest must contain exactly one sample {sample_id}")
    sample = matched[0]
    capture_ticks = sample.get("captureTicks")
    if not isinstance(capture_ticks, list) or not capture_ticks:
        raise ContractError("sample manifest captureTicks must be a non-empty array")
    declared_capture_ticks: list[int] = []
    previous_capture_tick = 0
    for index, capture_tick in enumerate(capture_ticks):
        value = _integer(capture_tick, f"sample manifest captureTicks[{index}]", 1)
        if value <= previous_capture_tick:
            raise ContractError("sample manifest captureTicks must be strictly increasing")
        declared_capture_ticks.append(value)
        previous_capture_tick = value
    roi_policy = sample.get("roiMetricPolicy")
    if not isinstance(roi_policy, dict):
        raise ContractError("sample manifest roiMetricPolicy must be an object")
    _require_keys(roi_policy, {"regionsOfInterest", "metrics"}, "sample manifest roiMetricPolicy")
    regions = roi_policy["regionsOfInterest"]
    if not isinstance(regions, list) or not regions:
        raise ContractError("sample manifest must declare at least one normalized ROI")
    normalized: dict[str, tuple[float, float, float, float]] = {}
    for index, region in enumerate(regions):
        context = f"sample manifest roiMetricPolicy.regionsOfInterest[{index}]"
        if not isinstance(region, dict):
            raise ContractError(f"{context} must be an object")
        _require_keys(region, {"id", "x", "y", "width", "height"}, context)
        identifier = _string(region["id"], f"{context}.id")
        if identifier in normalized:
            raise ContractError(f"sample manifest has duplicate normalized ROI id {identifier}")
        x = _unit_interval(region["x"], f"{context}.x")
        y = _unit_interval(region["y"], f"{context}.y")
        width = _number(region["width"], f"{context}.width", 0.0)
        height = _number(region["height"], f"{context}.height", 0.0)
        if width <= 0.0 or height <= 0.0 or x + width > 1.0 or y + height > 1.0:
            raise ContractError(f"{context} must be a contained, non-empty unit-space rectangle")
        normalized[identifier] = (x, y, width, height)
    return normalized, tuple(declared_capture_ticks)


def _resolve_roi(
    attachment: Attachment,
    roi: dict[str, Any],
    normalized_regions: dict[str, tuple[float, float, float, float]],
) -> dict[str, int]:
    region_id = str(roi["normalizedRegionId"])
    try:
        normalized_x, normalized_y, normalized_width, normalized_height = normalized_regions[region_id]
    except KeyError as error:
        raise ContractError(
            f"ROI {roi['id']} references unknown normalized sample-manifest ROI {region_id}"
        ) from error

    # The half-open unit rectangle is converted outward.  The exact use of
    # floor for its origin and ceil for its far edge is stable across all
    # attachments and resolutions, preserves every covered source pixel, and
    # does not depend on renderer-private scene or render-graph data.
    x = math.floor(normalized_x * attachment.width)
    y = math.floor(normalized_y * attachment.height)
    right = math.ceil((normalized_x + normalized_width) * attachment.width)
    bottom = math.ceil((normalized_y + normalized_height) * attachment.height)
    if x < 0 or y < 0 or right > attachment.width or bottom > attachment.height or right <= x or bottom <= y:
        raise ContractError(f"normalized ROI {region_id} maps outside {attachment.semantic} extent")
    expected = {"x": x, "y": y, "width": right - x, "height": bottom - y}
    actual = {field: int(roi[field]) for field in expected}
    if actual != expected:
        raise ContractError(
            f"ROI {roi['id']} does not match the deterministic normalized mapping for {attachment.semantic}"
        )
    return expected


def _roi_values(attachment: Attachment, resolved: dict[str, int], roi_id: str) -> tuple[float, ...]:
    x = resolved["x"]
    y = resolved["y"]
    width = resolved["width"]
    height = resolved["height"]
    if x + width > attachment.width or y + height > attachment.height:
        raise ContractError(f"ROI {roi_id} is outside {attachment.semantic} extent")
    values: list[float] = []
    row_values = attachment.width * attachment.channels
    for row in range(y, y + height):
        start = row * row_values + x * attachment.channels
        values.extend(attachment.values[start:start + width * attachment.channels])
    return tuple(values)


def _metrics(
    expected: Iterable[float],
    actual: Iterable[float],
    data_range: float,
) -> dict[str, float | None | bool]:
    pairs = list(zip(expected, actual, strict=True))
    if not pairs:
        raise ContractError("ROI contains no samples")
    errors = [abs(left - right) for left, right in pairs]
    mae = sum(errors) / len(errors)
    maximum = max(errors)
    mse = sum(error * error for error in errors) / len(errors)
    psnr = math.inf if mse == 0.0 else 20.0 * math.log10(data_range / math.sqrt(mse))
    left_values = [pair[0] for pair in pairs]
    right_values = [pair[1] for pair in pairs]
    left_mean = sum(left_values) / len(left_values)
    right_mean = sum(right_values) / len(right_values)
    left_variance = sum((value - left_mean) ** 2 for value in left_values) / len(left_values)
    right_variance = sum((value - right_mean) ** 2 for value in right_values) / len(right_values)
    covariance = sum((left - left_mean) * (right - right_mean) for left, right in pairs) / len(pairs)
    c1, c2 = 0.0001, 0.0009
    denominator = (left_mean * left_mean + right_mean * right_mean + c1) * (left_variance + right_variance + c2)
    ssim = 1.0 if denominator == 0.0 else ((2.0 * left_mean * right_mean + c1) * (2.0 * covariance + c2)) / denominator
    return {
        "mae": mae,
        "maxAbs": maximum,
        "psnr": None if math.isinf(psnr) else psnr,
        "psnrIsInfinite": math.isinf(psnr),
        "ssim": ssim,
    }


def _write_heatmap(path: Path, attachment: Attachment, expected: Attachment) -> None:
    # A PPM heatmap is intentionally a diagnostic, not a golden artifact.
    pixels: bytearray = bytearray()
    for pixel in range(attachment.width * attachment.height):
        start = pixel * attachment.channels
        error = max(abs(attachment.values[start + channel] - expected.values[start + channel]) for channel in range(attachment.channels))
        intensity = min(255, round(error * 255.0))
        pixels.extend((intensity, 0, 255 - intensity))
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(f"P6\n{attachment.width} {attachment.height}\n255\n".encode("ascii") + pixels)


def _comparison(
    actual: Bundle,
    baseline: Bundle,
    policy: dict[str, Any],
    normalized_regions: dict[str, tuple[float, float, float, float]],
    heatmap_dir: Path | None,
) -> tuple[list[dict[str, Any]], list[str]]:
    if actual.qualification_identity != baseline.qualification_identity:
        raise ContractError("bundle qualificationIdentity does not match baseline")
    if (
        actual.render_configuration_identity
        != baseline.render_configuration_identity
    ):
        raise ContractError(
            "bundle runtime configuration identity does not match baseline"
        )
    if {
        key: value
        for key, value in actual.qualification_identity.items()
        if key != "captureTick"
    } != policy["qualificationIdentity"]:
        raise ContractError("bundle qualificationIdentity does not match visual policy")
    output: list[dict[str, Any]] = []
    failures: list[str] = []
    for semantic in REQUIRED_SEMANTICS:
        current = actual.attachments[semantic]
        expected = baseline.attachments[semantic]
        if (current.width, current.height, current.channels, current.format, current.color_domain) != (
            expected.width, expected.height, expected.channels, expected.format, expected.color_domain,
        ):
            raise ContractError(f"{semantic} metadata does not match baseline")
        if heatmap_dir:
            _write_heatmap(heatmap_dir / f"{semantic}.diff.ppm", current, expected)
        for roi in policy["attachments"][semantic]["rois"]:
            current_roi = _resolve_roi(current, roi, normalized_regions)
            expected_roi = _resolve_roi(expected, roi, normalized_regions)
            metric = _metrics(
                _roi_values(expected, expected_roi, str(roi["id"])),
                _roi_values(current, current_roi, str(roi["id"])),
                float(roi["dataRange"]),
            )
            passed = (
                metric["mae"] <= float(roi["maxMae"])
                and metric["maxAbs"] <= float(roi["maxAbs"])
                and (metric["psnrIsInfinite"] or float(metric["psnr"]) >= float(roi["minPsnr"]))
                and metric["ssim"] >= float(roi["minSsim"])
            )
            record = {
                "semantic": semantic,
                "roi": roi["id"],
                "normalizedRegionId": roi["normalizedRegionId"],
                "pixelROI": current_roi,
                "passed": passed,
                **metric,
            }
            output.append(record)
            if not passed:
                failures.append(f"visual metric threshold failed for {semantic}/{roi['id']}")
    return output, failures


def _temporal(
    current: list[Bundle],
    policy: dict[str, Any],
    normalized_regions: dict[str, tuple[float, float, float, float]],
    declared_capture_ticks: tuple[int, ...],
    expected_source_commit: str,
    expected_run_id: str,
    expected_preset: str,
) -> tuple[list[dict[str, Any]], list[str]]:
    if not current:
        return [], []
    warmup = int(policy["temporal"]["warmupCompletedFrames"])
    expected_ticks = tuple(int(value) for value in policy["temporal"]["adjacentCaptureTicks"])
    if len(current) != 2:
        raise ContractError("temporal validation requires exactly two adjacent bundles")
    identity = current[0].qualification_identity
    configuration = current[0].render_configuration_identity
    non_temporal_identity = {
        key: value for key, value in identity.items() if key != "captureTick"
    }
    previous_completed = -1
    previous_submitted = -1
    previous_sequence = 0
    previous_capture_tick = 0
    observed_bundle_ids: set[str] = set()
    observed_receipts: set[str] = set()
    for bundle in current:
        bundle_non_temporal_identity = {
            key: value
            for key, value in bundle.qualification_identity.items()
            if key != "captureTick"
        }
        if bundle_non_temporal_identity != non_temporal_identity:
            raise ContractError("temporal bundles differ outside captureTick identity")
        if bundle.render_configuration_identity != configuration:
            raise ContractError(
                "temporal bundles differ in runtime configuration identity"
            )
        if bundle_non_temporal_identity != policy["qualificationIdentity"]:
            raise ContractError("temporal bundle qualificationIdentity does not match visual policy")
        if bundle.source_commit != expected_source_commit:
            raise ContractError("temporal bundle sourceCommit is stale for the requested qualification")
        if bundle.execution["runId"] != expected_run_id:
            raise ContractError("temporal bundle runId does not match the requested qualification run")
        if bundle.execution["preset"] != expected_preset:
            raise ContractError("temporal bundle preset does not match the requested qualification preset")
        if bundle.bundle_id in observed_bundle_ids:
            raise ContractError("temporal bundles reuse a bundleId")
        observed_bundle_ids.add(bundle.bundle_id)
        receipt = str(bundle.execution["renderExecutionReceiptSha256"])
        if receipt in observed_receipts:
            raise ContractError("temporal bundles reuse a render execution receipt")
        observed_receipts.add(receipt)
        capture_tick = int(bundle.qualification_identity["captureTick"])
        if capture_tick not in declared_capture_ticks:
            raise ContractError("temporal bundle captureTick is not declared by the sample manifest")
        if capture_tick <= previous_capture_tick:
            raise ContractError("temporal bundle capture ticks are stale or out of order")
        frame_sequence = int(bundle.execution["frameSequence"])
        submitted = int(bundle.execution["submittedFrame"])
        completed = int(bundle.execution["completedFrame"])
        if frame_sequence < capture_tick:
            raise ContractError("temporal bundle frameSequence predates its declared captureTick")
        if completed < warmup:
            raise ContractError("temporal bundle was captured before the policy warmup completed")
        if frame_sequence <= previous_sequence:
            raise ContractError("temporal bundle frame sequences are stale or out of order")
        if submitted <= previous_submitted:
            raise ContractError("temporal bundle submitted frames are stale or out of order")
        if completed <= previous_completed:
            raise ContractError("temporal bundle completion frames are stale or out of order")
        previous_sequence = frame_sequence
        previous_submitted = submitted
        previous_completed = completed
        previous_capture_tick = capture_tick
    actual_ticks = tuple(
        int(bundle.qualification_identity["captureTick"]) for bundle in current
    )
    if actual_ticks != expected_ticks:
        raise ContractError(
            "temporal bundles do not match the policy adjacentCaptureTicks"
        )
    output: list[dict[str, Any]] = []
    failures: list[str] = []
    for semantic in REQUIRED_SEMANTICS:
        for roi in policy["attachments"][semantic]["rois"]:
            older, newer = current
            before_attachment = older.attachments[semantic]
            after_attachment = newer.attachments[semantic]
            before = _roi_values(
                before_attachment,
                _resolve_roi(before_attachment, roi, normalized_regions),
                str(roi["id"]),
            )
            after = _roi_values(
                after_attachment,
                _resolve_roi(after_attachment, roi, normalized_regions),
                str(roi["id"]),
            )
            mean_delta = sum(
                abs(left - right)
                for left, right in zip(before, after, strict=True)
            ) / len(before)
            minimum_delta = float(roi["minTemporalMae"])
            maximum_delta = float(roi["maxTemporalMae"])
            passed = mean_delta <= maximum_delta and (
                roi["temporalMode"] != "dynamic"
                or mean_delta >= minimum_delta
            )
            record = {
                "semantic": semantic, "roi": roi["id"], "temporalMode": roi["temporalMode"],
                "normalizedRegionId": roi["normalizedRegionId"],
                "meanTemporalMae": mean_delta,
                "minTemporalMae": minimum_delta,
                "maxTemporalMae": maximum_delta,
                "passed": passed,
            }
            output.append(record)
            if not passed:
                failures.append(
                    f"temporal motion/flicker threshold failed for {semantic}/{roi['id']}"
                )
    return output, failures


def _write_report(path: Path | None, report: dict[str, Any]) -> None:
    if path is None:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(report, indent=2, sort_keys=True, allow_nan=False) + "\n", encoding="utf-8")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bundle", required=True, type=Path, help="current attachment bundle manifest")
    parser.add_argument("--baseline", required=True, type=Path, help="approved baseline attachment bundle manifest")
    parser.add_argument(
        "--golden-record",
        required=True,
        type=Path,
        help="approved GoldenRecord V1 binding the baseline, policy, and manifest",
    )
    parser.add_argument("--policy", required=True, type=Path, help="versioned ROI/metric policy JSON")
    parser.add_argument(
        "--sample-manifest",
        required=True,
        type=Path,
        help="public sample manifest supplying normalized ROIs",
    )
    parser.add_argument("--temporal-bundle", action="append", default=[], type=Path, help="ordered completion-owned bundle after warmup; repeat")
    parser.add_argument("--expected-source-commit", required=True, help="fail if the current bundle was captured from another commit")
    parser.add_argument("--expected-run-id", required=True, help="fail if the current bundle belongs to another capture run")
    parser.add_argument("--expected-preset", required=True, help="fail if a bundle belongs to another configure/test preset")
    parser.add_argument("--expected-receipt-sha256", required=True, help="fail if the current bundle receipt differs")
    parser.add_argument("--heatmap-dir", type=Path, help="diagnostic PPM heatmap output directory")
    parser.add_argument("--report", type=Path, help="JSON result path")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    report: dict[str, Any] = {
        "schema": "RVX.RenderQualityVisualResult",
        "schemaVersion": 2,
        "status": "failed",
        "failures": [],
        "metrics": [],
        "temporal": [],
    }
    try:
        current = load_bundle(args.bundle)
        baseline = load_bundle(args.baseline)
        policy = load_policy(args.policy)
        normalized_regions, declared_capture_ticks = load_sample_contract(
            args.sample_manifest,
            str(policy["qualificationIdentity"]["sampleId"]),
        )
        golden_tick = int(policy["temporal"]["goldenCaptureTick"])
        if golden_tick not in declared_capture_ticks:
            raise ContractError(
                "visual policy goldenCaptureTick is not declared by the sample manifest"
            )
        current_identity = {
            key: value
            for key, value in current.qualification_identity.items()
            if key != "captureTick"
        }
        baseline_identity = {
            key: value
            for key, value in baseline.qualification_identity.items()
            if key != "captureTick"
        }
        golden_tick = int(policy["temporal"]["goldenCaptureTick"])
        if (
            current_identity != policy["qualificationIdentity"]
            or baseline_identity != policy["qualificationIdentity"]
            or int(current.qualification_identity["captureTick"]) != golden_tick
            or int(baseline.qualification_identity["captureTick"]) != golden_tick
        ):
            raise ContractError(
                "Golden bundle identity or capture tick does not match visual policy"
            )
        if args.expected_source_commit and current.source_commit != args.expected_source_commit:
            raise ContractError("bundle sourceCommit is stale for the requested qualification")
        if args.expected_run_id and current.execution["runId"] != args.expected_run_id:
            raise ContractError("bundle runId does not match the requested qualification run")
        if current.execution["preset"] != args.expected_preset:
            raise ContractError("bundle preset does not match the requested qualification preset")
        if args.expected_receipt_sha256 and current.execution["renderExecutionReceiptSha256"] != args.expected_receipt_sha256:
            raise ContractError("bundle receipt does not match the requested qualification receipt")
        metrics, failures = _comparison(
            current,
            baseline,
            policy,
            normalized_regions,
            args.heatmap_dir,
        )
        try:
            from render_quality_golden import (
                GoldenError,
                verify_golden_record,
            )

            verify_golden_record(
                args.golden_record,
                args.baseline,
                args.policy,
                args.sample_manifest,
                require_approved=True,
            )
        except GoldenError as error:
            raise ContractError(str(error)) from error
        if len(args.temporal_bundle) != 2:
            raise ContractError(
                "exactly two --temporal-bundle values are required for the adjacent pair"
            )
        temporal_paths = list(args.temporal_bundle)
        temporal_bundles = [load_bundle(path) for path in temporal_paths]
        temporal, temporal_failures = _temporal(
            temporal_bundles,
            policy,
            normalized_regions,
            declared_capture_ticks,
            args.expected_source_commit,
            args.expected_run_id,
            args.expected_preset,
        )
        failures.extend(temporal_failures)
        report.update({
            "currentBundle": str(args.bundle), "baselineBundle": str(args.baseline),
            "bundleId": current.bundle_id, "sourceCommit": current.source_commit,
            "qualificationIdentity": current.qualification_identity,
            "renderConfigurationIdentity": current.render_configuration_identity,
            "execution": current.execution, "metrics": metrics, "temporal": temporal,
            "failures": failures, "status": "passed" if not failures else "failed",
        })
        _write_report(args.report, report)
        if failures:
            print("Render quality visual qualification failed:", *failures, sep="\n  - ")
            return 1
        print("Render quality visual qualification passed")
        return 0
    except ContractError as error:
        report["failures"] = [str(error)]
        _write_report(args.report, report)
        print(f"Render quality visual qualification rejected: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
