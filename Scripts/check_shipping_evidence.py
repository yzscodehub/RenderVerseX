#!/usr/bin/env python3
"""Validate cooked-only Shipping runtime evidence and its fixture receipt."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
from pathlib import Path, PurePosixPath
from typing import Any

from architecture_gate_common import (
    GateInputError,
    resolve_repo_root,
    resolve_required_file,
    run_gate,
)


REPORT_SCHEMA = "RVX_SHIPPING_EVIDENCE"
RECEIPT_SCHEMA = "RVX_SHIPPING_FIXTURE_RECEIPT"
BACKENDS = frozenset({"DX11", "DX12", "Vulkan", "Metal", "OpenGL"})
SHA256_PATTERN = re.compile(r"^[0-9a-f]{64}$")
SAFE_ARTIFACT_PATTERN = re.compile(r"^Cooked/[A-Za-z0-9._/-]+$")

RECEIPT_FIELDS = frozenset({
    "schema",
    "schemaVersion",
    "commit",
    "preset",
    "backend",
    "sourceModel",
    "sourceModelSha256",
    "shaderCatalog",
    "shaderCatalogSha256",
    "cookExecutableSha256",
    "resourceManifest",
    "resourceEntryCount",
    "shaderProgramCount",
    "artifactCount",
    "artifactPayloadHash",
    "artifacts",
})

REPORT_FIELDS = frozenset({
    "schema",
    "schemaVersion",
    "commit",
    "preset",
    "frames",
    "width",
    "height",
    "quality",
    "renderPath",
    "requestedBackend",
    "actualBackend",
    "fallbackUsed",
    "fallbackReason",
    "softwareAdapterAllowed",
    "softwareAdapter",
    "adapterName",
    "cookedLoadSucceeded",
    "sourceLoadRejected",
    "shutdownClean",
    "fixtureReceiptSha256",
    "fixtureArtifactPayloadHash",
})


def _load_json(path: Path, label: str) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except UnicodeDecodeError as error:
        raise GateInputError(f"{label} must be UTF-8: {error}") from error
    except json.JSONDecodeError as error:
        raise GateInputError(f"{label} is invalid JSON: {error}") from error
    if not isinstance(value, dict):
        raise GateInputError(f"{label} root must be an object")
    return value


def _string(value: Any, context: str) -> str:
    if not isinstance(value, str) or not value:
        raise GateInputError(f"{context} must be a non-empty string")
    return value


def _integer(value: Any, context: str, minimum: int = 0) -> int:
    if type(value) is not int or value < minimum:
        raise GateInputError(f"{context} must be an integer >= {minimum}")
    return value


def _boolean(value: Any, context: str) -> bool:
    if type(value) is not bool:
        raise GateInputError(f"{context} must be a boolean")
    return value


def _sha256(value: Any, context: str) -> str:
    text = _string(value, context)
    if not SHA256_PATTERN.fullmatch(text):
        raise GateInputError(f"{context} must be a lowercase SHA-256 digest")
    return text


def _file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while chunk := stream.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def _resolve_artifact(receipt_path: Path, raw_path: Any) -> tuple[str, Path]:
    relative = _string(raw_path, "receipt artifact path")
    if not SAFE_ARTIFACT_PATTERN.fullmatch(relative):
        raise GateInputError(f"receipt artifact path is unsafe: {relative}")
    pure = PurePosixPath(relative)
    if pure.is_absolute() or ".." in pure.parts or "." in pure.parts:
        raise GateInputError(
            f"receipt artifact path is unsafe or not canonical: {relative}"
        )
    fixture_root = receipt_path.parent.resolve()
    candidate = fixture_root / Path(*pure.parts)
    if candidate.is_symlink():
        raise GateInputError(f"receipt artifact must not be a symbolic link: {relative}")
    resolved = candidate.resolve()
    try:
        resolved.relative_to(fixture_root)
    except ValueError as error:
        raise GateInputError(
            f"receipt artifact escapes the fixture root: {relative}"
        ) from error
    if not resolved.is_file():
        raise GateInputError(f"receipt artifact does not exist: {relative}")
    return relative, resolved


def validate_receipt(receipt_path: Path) -> tuple[dict[str, Any], list[str]]:
    receipt = _load_json(receipt_path, "Shipping fixture receipt")
    if set(receipt) != RECEIPT_FIELDS:
        missing = sorted(RECEIPT_FIELDS - set(receipt))
        unknown = sorted(set(receipt) - RECEIPT_FIELDS)
        raise GateInputError(
            f"Shipping fixture receipt fields are invalid; missing={missing}, unknown={unknown}"
        )
    if receipt["schema"] != RECEIPT_SCHEMA or receipt["schemaVersion"] != 1:
        raise GateInputError(
            f"Shipping fixture receipt must use {RECEIPT_SCHEMA} schemaVersion 1"
        )
    commit = _string(receipt["commit"], "receipt.commit")
    if not re.fullmatch(r"[0-9A-Fa-f]{7,64}", commit):
        raise GateInputError("receipt.commit is not a hexadecimal commit id")
    preset = _string(receipt["preset"], "receipt.preset")
    if not re.fullmatch(r"[A-Za-z0-9_.-]+", preset):
        raise GateInputError("receipt.preset is not a canonical preset id")
    backend = _string(receipt["backend"], "receipt.backend")
    if backend not in BACKENDS:
        raise GateInputError(f"receipt.backend is unsupported: {backend}")
    source_model = _string(receipt["sourceModel"], "receipt.sourceModel")
    if source_model != "Tests/Fixtures/ModelViewer/R7Triangle.gltf":
        raise GateInputError("receipt.sourceModel is not the hermetic R7Triangle fixture")
    _sha256(receipt["sourceModelSha256"], "receipt.sourceModelSha256")
    shader_catalog = _string(receipt["shaderCatalog"], "receipt.shaderCatalog")
    if shader_catalog != "Source/Runtime/Render/Shaders/Programs.rvxshadercatalog.json":
        raise GateInputError("receipt.shaderCatalog is not the repository shader catalog")
    _sha256(receipt["shaderCatalogSha256"], "receipt.shaderCatalogSha256")
    _sha256(receipt["cookExecutableSha256"], "receipt.cookExecutableSha256")
    resource_manifest = _string(
        receipt["resourceManifest"], "receipt.resourceManifest"
    )
    if resource_manifest != "Cooked/Resources/CookManifest.rvxmanifest":
        raise GateInputError("receipt.resourceManifest is not canonical")
    resource_entries = _integer(
        receipt["resourceEntryCount"], "receipt.resourceEntryCount", 1
    )
    shader_programs = _integer(
        receipt["shaderProgramCount"], "receipt.shaderProgramCount", 1
    )
    artifact_count = _integer(
        receipt["artifactCount"], "receipt.artifactCount", 3
    )
    expected_payload_hash = _sha256(
        receipt["artifactPayloadHash"], "receipt.artifactPayloadHash"
    )

    artifacts = receipt["artifacts"]
    if not isinstance(artifacts, list) or len(artifacts) != artifact_count:
        raise GateInputError(
            "receipt.artifacts length does not match receipt.artifactCount"
        )
    paths: list[str] = []
    payload_lines: list[str] = []
    shader_count = 0
    model_artifact_count = 0
    for index, artifact in enumerate(artifacts):
        context = f"receipt.artifacts[{index}]"
        if not isinstance(artifact, dict) or set(artifact) != {
            "path", "size", "sha256"
        }:
            raise GateInputError(
                f"{context} must contain exactly path, size, and sha256"
            )
        relative, path = _resolve_artifact(receipt_path, artifact["path"])
        size = _integer(artifact["size"], f"{context}.size", 1)
        digest = _sha256(artifact["sha256"], f"{context}.sha256")
        if path.stat().st_size != size:
            raise GateInputError(f"{relative} size does not match its receipt")
        if _file_sha256(path) != digest:
            raise GateInputError(f"{relative} SHA-256 does not match its receipt")
        paths.append(relative)
        payload_lines.append(f"{relative}|{size}|{digest}\n")
        shader_count += relative.endswith(".rvxshader")
        model_artifact_count += (
            relative.startswith("Cooked/Resources/")
            and relative.endswith(".rva")
        )

    if paths != sorted(paths) or len(paths) != len(set(paths)):
        raise GateInputError("receipt artifact paths must be sorted and unique")
    cooked_root = receipt_path.parent.resolve() / "Cooked"
    if not cooked_root.is_dir() or cooked_root.is_symlink():
        raise GateInputError("fixture Cooked root is missing or is a symbolic link")
    actual_paths: list[str] = []
    for path in cooked_root.rglob("*"):
        if path.is_symlink():
            raise GateInputError(f"fixture Cooked tree contains symbolic link: {path}")
        if path.is_file():
            actual_paths.append(path.relative_to(receipt_path.parent.resolve()).as_posix())
    if sorted(actual_paths) != paths:
        raise GateInputError(
            "receipt artifact inventory does not exactly match the Cooked tree"
        )
    if resource_manifest not in paths:
        raise GateInputError("receipt resource manifest is absent from artifacts")
    if "Cooked/Resources/R7Triangle.rva" not in paths:
        raise GateInputError("receipt is missing Cooked/Resources/R7Triangle.rva")
    if shader_count != shader_programs:
        raise GateInputError(
            "receipt shader artifact count does not match shaderProgramCount"
        )
    expected_shader_prefix = f"Cooked/Shaders/{backend}/"
    if any(
        path.endswith(".rvxshader") and not path.startswith(expected_shader_prefix)
        for path in paths
    ):
        raise GateInputError("receipt shader artifact backend directory is inconsistent")
    source_suffixes = (
        ".fbx", ".glb", ".gltf", ".hdr", ".obj", ".png", ".tga", ".wav"
    )
    if any(path.casefold().endswith(source_suffixes) for path in paths):
        raise GateInputError("fixture Cooked tree contains a source-format artifact")
    if model_artifact_count < resource_entries:
        raise GateInputError(
            "receipt has fewer cooked resource artifacts than resource entries"
        )
    actual_payload_hash = hashlib.sha256(
        "".join(payload_lines).encode("utf-8")
    ).hexdigest()
    if actual_payload_hash != expected_payload_hash:
        raise GateInputError("receipt artifactPayloadHash is inconsistent")
    return receipt, paths


def validate_shipping_evidence(
    report_path: Path,
    receipt_path: Path,
    *,
    repository_root: Path | None = None,
    expected_backend: str | None = None,
    expected_commit: str | None = None,
    expected_preset: str | None = None,
) -> list[str]:
    receipt, _ = validate_receipt(receipt_path)
    report = _load_json(report_path, "Shipping evidence report")
    if set(report) != REPORT_FIELDS:
        missing = sorted(REPORT_FIELDS - set(report))
        unknown = sorted(set(report) - REPORT_FIELDS)
        raise GateInputError(
            f"Shipping evidence report fields are invalid; missing={missing}, unknown={unknown}"
        )
    if report["schema"] != REPORT_SCHEMA or report["schemaVersion"] != 2:
        raise GateInputError(
            f"Shipping evidence report must use {REPORT_SCHEMA} schemaVersion 2"
        )

    commit = _string(report["commit"], "report.commit")
    preset = _string(report["preset"], "report.preset")
    requested_backend = _string(
        report["requestedBackend"], "report.requestedBackend"
    )
    actual_backend = _string(report["actualBackend"], "report.actualBackend")
    adapter_name = _string(report["adapterName"], "report.adapterName")
    software_adapter_allowed = _boolean(
        report["softwareAdapterAllowed"],
        "report.softwareAdapterAllowed",
    )
    software_adapter = _boolean(
        report["softwareAdapter"],
        "report.softwareAdapter",
    )
    receipt_sha256 = _sha256(
        report["fixtureReceiptSha256"], "report.fixtureReceiptSha256"
    )
    report_payload_hash = _sha256(
        report["fixtureArtifactPayloadHash"],
        "report.fixtureArtifactPayloadHash",
    )

    findings: list[str] = []
    exact_values = {
        "frames": 8,
        "width": 320,
        "height": 180,
        "quality": "Low",
        "renderPath": "Direct",
        "fallbackUsed": False,
        "fallbackReason": "",
        "cookedLoadSucceeded": True,
        "sourceLoadRejected": True,
        "shutdownClean": True,
    }
    for name, expected in exact_values.items():
        actual = report[name]
        if type(expected) is bool:
            _boolean(actual, f"report.{name}")
        elif type(expected) is int:
            _integer(actual, f"report.{name}")
        elif not isinstance(actual, str):
            raise GateInputError(f"report.{name} must be a string")
        if actual != expected:
            findings.append(
                f"report.{name} must be {expected!r}, observed {actual!r}"
            )

    if requested_backend not in BACKENDS:
        findings.append(f"requested backend is unsupported: {requested_backend}")
    if actual_backend != requested_backend:
        findings.append(
            f"backend fallback is forbidden: requested={requested_backend}, actual={actual_backend}"
        )
    if software_adapter and not software_adapter_allowed:
        findings.append(
            f"software adapter {adapter_name!r} was used without explicit opt-in"
        )
    comparisons = (
        ("commit", commit, receipt["commit"]),
        ("preset", preset, receipt["preset"]),
        ("backend", requested_backend, receipt["backend"]),
        (
            "artifact payload hash",
            report_payload_hash,
            receipt["artifactPayloadHash"],
        ),
        ("receipt SHA-256", receipt_sha256, _file_sha256(receipt_path)),
    )
    for label, actual, expected in comparisons:
        if actual != expected:
            findings.append(
                f"Shipping evidence {label} mismatch: report={actual}, expected={expected}"
            )
    if repository_root is not None:
        source_root = repository_root.resolve()
        for field, hash_field, label in (
            ("sourceModel", "sourceModelSha256", "source model"),
            ("shaderCatalog", "shaderCatalogSha256", "shader catalog"),
        ):
            relative = PurePosixPath(receipt[field])
            path = (source_root / Path(*relative.parts)).resolve()
            try:
                path.relative_to(source_root)
            except ValueError as error:
                raise GateInputError(f"receipt {label} escapes repository root") from error
            if not path.is_file():
                raise GateInputError(f"receipt {label} does not exist: {path}")
            observed_hash = _file_sha256(path)
            if observed_hash != receipt[hash_field]:
                findings.append(
                    f"Shipping fixture {label} hash drifted: receipt={receipt[hash_field]}, repository={observed_hash}"
                )
    if expected_backend is not None and requested_backend != expected_backend:
        findings.append(
            f"requested backend {requested_backend} does not match gate backend {expected_backend}"
        )
    if expected_commit is not None and commit != expected_commit:
        findings.append(
            f"report commit {commit} does not match gate commit {expected_commit}"
        )
    if expected_preset is not None and preset != expected_preset:
        findings.append(
            f"report preset {preset} does not match gate preset {expected_preset}"
        )
    return sorted(set(findings))


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", default=".", help="Repository root.")
    parser.add_argument("--report", required=True, help="Shipping evidence JSON.")
    parser.add_argument("--receipt", required=True, help="Fixture receipt JSON.")
    parser.add_argument("--expected-backend", choices=sorted(BACKENDS))
    parser.add_argument("--expected-commit")
    parser.add_argument("--expected-preset")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    root = resolve_repo_root(args.root)
    report = resolve_required_file(root, args.report, "Shipping evidence report")
    receipt = resolve_required_file(root, args.receipt, "Shipping fixture receipt")
    findings = validate_shipping_evidence(
        report,
        receipt,
        repository_root=root,
        expected_backend=args.expected_backend,
        expected_commit=args.expected_commit,
        expected_preset=args.expected_preset,
    )
    if findings:
        print("Shipping evidence violations:")
        for finding in findings:
            print("  " + finding)
        return 1
    print("Shipping evidence passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(run_gate(main))
