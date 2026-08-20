#!/usr/bin/env python3
"""Plan dependency-aware validation without weakening required PR gates.

The planner is intentionally observation-only in CI until its recorded history
contains at least ten code pull requests and three Nightly runs with no misses.
Unknown or policy-sensitive changes always select the full matrix.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import subprocess
import sys
from collections import defaultdict, deque
from pathlib import Path, PurePosixPath
from typing import Any, Iterable

from architecture_gate_common import GateInputError, resolve_repo_root, run_gate
from module_policy import Policy, add_policy_arguments, load_policy


PLAN_SCHEMA = "RVX.ValidationSelectionPlan"
EVIDENCE_SCHEMA = "RVX.ValidationPlannerEvidence"
MIN_CODE_PULL_REQUESTS = 10
MIN_NIGHTLIES = 3
REQUIRED_JOBS = (
    "architecture",
    "windows-debug",
    "linux-debug",
    "linux-tsan",
    "macos-arm64",
    "shipping",
)
PRIMARY_BACKENDS = ("DX12", "Vulkan", "Metal")
COMPATIBILITY_BACKENDS = ("DX11", "OpenGL")
GLOBAL_FILES = {
    "CMakeLists.txt",
    "CMakePresets.json",
    "vcpkg.json",
    "vcpkg-configuration.json",
    "Docs/module-boundaries.json",
}
GLOBAL_PREFIXES = (
    ".github/",
    "cmake/",
    "vcpkg-ports/",
)
POLICY_SCRIPT_PREFIXES = (
    "Scripts/check_",
    "Scripts/module_policy.py",
    "Scripts/plan_validation.py",
    "Scripts/run_build_truth",
    "Scripts/run_shipping",
    "Scripts/test_validation_planner.py",
)
PLATFORM_SENSITIVE_MODULES = {
    "Core",
    "Engine",
    "HAL",
    "Render",
    "RenderContracts",
    "RHI",
    "RHI_BackendFactory",
    "RHI_DX11",
    "RHI_DX12",
    "RHI_Metal",
    "RHI_OpenGL",
    "RHI_Vulkan",
    "RuntimeServices",
    "ShaderContracts",
    "ShaderRuntime",
}
COMPATIBILITY_SENSITIVE_MODULES = {
    "Core",
    "Engine",
    "HAL",
    "Render",
    "RenderContracts",
    "RHI",
    "RHI_BackendFactory",
    "RuntimeServices",
    "ShaderContracts",
    "ShaderRuntime",
}


def normalize_changed_file(value: str) -> str:
    text = value.strip().replace("\\", "/")
    path = PurePosixPath(text)
    if (not text or path.is_absolute() or text.startswith("/")
            or any(part in {"", ".", ".."} for part in path.parts)):
        raise GateInputError(f"changed file is not a safe repository-relative path: {value!r}")
    return path.as_posix()


def discover_changed_files(root: Path, base_ref: str, head_ref: str) -> list[str]:
    if not base_ref.strip() or not head_ref.strip():
        raise GateInputError("base and head refs must be non-empty")
    command = [
        "git", "-c", f"safe.directory={root.as_posix()}",
        "-c", "core.quotePath=false", "diff",
        "--name-only", "-z", "--diff-filter=ACDMRTUXB", f"{base_ref}...{head_ref}",
    ]
    try:
        result = subprocess.run(
            command,
            cwd=root,
            check=False,
            capture_output=True,
            text=True,
            encoding="utf-8",
        )
    except OSError as error:
        raise GateInputError(f"cannot execute git change discovery: {error}") from error
    if result.returncode != 0:
        diagnostic = result.stderr.strip() or result.stdout.strip() or "unknown git error"
        raise GateInputError(f"git change discovery failed: {diagnostic}")
    return sorted({normalize_changed_file(item) for item in result.stdout.split("\0") if item})


def reverse_module_closure(policy: Policy, changed_modules: Iterable[str]) -> list[str]:
    reverse: dict[str, set[str]] = defaultdict(set)
    active = policy.active_module_ids()
    for module_id in active:
        for dependency in policy.modules[module_id].allowed_modules:
            if dependency in active:
                reverse[dependency].add(module_id)
    visited = set(changed_modules)
    queue = deque(sorted(visited))
    while queue:
        current = queue.popleft()
        for consumer in sorted(reverse.get(current, ())):
            if consumer not in visited:
                visited.add(consumer)
                queue.append(consumer)
    return sorted(visited)


def classify_changes(policy: Policy, changed_files: Iterable[str]) -> dict[str, Any]:
    modules: set[str] = set()
    unknown: list[str] = []
    global_sensitive: list[str] = []
    documentation: list[str] = []
    platform_paths: list[str] = []
    for relative in sorted(set(changed_files)):
        if (relative in GLOBAL_FILES or relative.startswith(GLOBAL_PREFIXES)
                or relative.startswith(POLICY_SCRIPT_PREFIXES)):
            global_sensitive.append(relative)
            continue
        module_id = policy.module_for_path(policy.root / relative)
        if module_id is not None:
            modules.add(module_id)
            lowered = relative.casefold()
            if (lowered.endswith(".mm") or "/platform/" in lowered
                    or "/windows/" in lowered or "/linux/" in lowered
                    or "/macos/" in lowered or "/apple/" in lowered):
                platform_paths.append(relative)
            continue
        if relative.startswith("Docs/") or relative in {"README.md", "AGENTS.md"}:
            documentation.append(relative)
        else:
            unknown.append(relative)
    return {
        "changedModules": sorted(modules),
        "documentationFiles": documentation,
        "globalSensitiveFiles": global_sensitive,
        "platformSensitiveFiles": platform_paths,
        "unknownFiles": unknown,
    }


def _read_evidence(path: Path | None) -> dict[str, Any]:
    if path is None:
        return {
            "qualifiedCodePullRequests": 0,
            "qualifiedNightlies": 0,
            "plannerMisses": 0,
            "qualified": False,
        }
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except OSError as error:
        raise GateInputError(f"cannot read planner evidence {path}: {error}") from error
    except json.JSONDecodeError as error:
        raise GateInputError(f"planner evidence is invalid JSON: {error}") from error
    expected = {
        "schema", "schemaVersion", "qualifiedCodePullRequests",
        "qualifiedNightlies", "plannerMisses",
    }
    if not isinstance(data, dict) or set(data) != expected:
        raise GateInputError("planner evidence must contain exactly the v1 evidence fields")
    if data["schema"] != EVIDENCE_SCHEMA or data["schemaVersion"] != 1:
        raise GateInputError(f"planner evidence must use {EVIDENCE_SCHEMA} schemaVersion 1")
    for field in ("qualifiedCodePullRequests", "qualifiedNightlies", "plannerMisses"):
        if not isinstance(data[field], int) or isinstance(data[field], bool) or data[field] < 0:
            raise GateInputError(f"planner evidence {field} must be a non-negative integer")
    return {
        **{field: data[field] for field in expected if field not in {"schema", "schemaVersion"}},
        "qualified": (
            data["qualifiedCodePullRequests"] >= MIN_CODE_PULL_REQUESTS
            and data["qualifiedNightlies"] >= MIN_NIGHTLIES
            and data["plannerMisses"] == 0
        ),
    }


def build_plan(
    policy: Policy,
    changed_files: Iterable[str],
    *,
    base_ref: str,
    head_ref: str,
    mode: str,
    evidence: dict[str, Any],
) -> dict[str, Any]:
    normalized = sorted({normalize_changed_file(item) for item in changed_files})
    classification = classify_changes(policy, normalized)
    changed_modules = classification["changedModules"]
    affected_modules = reverse_module_closure(policy, changed_modules)
    global_or_unknown = bool(
        classification["globalSensitiveFiles"] or classification["unknownFiles"]
    )
    no_changes = not normalized
    platform_sensitive = bool(
        global_or_unknown or no_changes
        or classification["platformSensitiveFiles"]
        or PLATFORM_SENSITIVE_MODULES.intersection(affected_modules)
    )

    compatibility: set[str] = set()
    if global_or_unknown or no_changes:
        compatibility.update(COMPATIBILITY_BACKENDS)
    else:
        if "RHI_DX11" in affected_modules:
            compatibility.add("DX11")
        if "RHI_OpenGL" in affected_modules:
            compatibility.add("OpenGL")
        if COMPATIBILITY_SENSITIVE_MODULES.intersection(changed_modules):
            compatibility.update(COMPATIBILITY_BACKENDS)

    if mode == "observation":
        full_reason = "observation-mode"
    elif global_or_unknown:
        full_reason = "unknown-or-global-change"
    elif no_changes:
        full_reason = "empty-change-set"
    elif not evidence["qualified"]:
        full_reason = "observation-threshold-not-met"
    else:
        full_reason = "dependency-selection-qualified"
    run_full_matrix = full_reason != "dependency-selection-qualified"

    plan: dict[str, Any] = {
        "schema": PLAN_SCHEMA,
        "schemaVersion": 1,
        "mode": mode,
        "baseRef": base_ref,
        "headRef": head_ref,
        "changedFiles": normalized,
        **classification,
        "affectedModules": affected_modules,
        "proposedSelection": {
            "portableUnit": {
                "Linux": "full",
                "Windows": (
                    "full" if global_or_unknown or no_changes
                    else "platform-sensitive-affected" if platform_sensitive
                    else "none"
                ),
                "macOS": (
                    "full" if global_or_unknown or no_changes
                    else "platform-sensitive-affected" if platform_sensitive
                    else "none"
                ),
            },
            "compatibilityBackends": sorted(compatibility),
            "primaryBackends": list(PRIMARY_BACKENDS),
            "integrationRequired": True,
            "shippingPlatforms": ["Windows", "Linux", "macOS"],
        },
        "enforcement": {
            "runFullMatrix": run_full_matrix,
            "reason": full_reason,
            "requiredJobs": list(REQUIRED_JOBS),
            "thresholds": {
                "codePullRequests": MIN_CODE_PULL_REQUESTS,
                "nightlies": MIN_NIGHTLIES,
                "plannerMisses": 0,
            },
            "observed": evidence,
        },
    }
    canonical = json.dumps(plan, ensure_ascii=False, sort_keys=True, separators=(",", ":"))
    plan["planHash"] = hashlib.sha256(canonical.encode("utf-8")).hexdigest()
    return plan


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", default=".", help="Repository root.")
    add_policy_arguments(parser)
    parser.add_argument("--base-ref", default="origin/master")
    parser.add_argument("--head-ref", default="HEAD")
    parser.add_argument("--changed-file", action="append", default=[])
    parser.add_argument("--mode", choices=("observation", "selective"), default="observation")
    parser.add_argument("--observation-evidence", type=Path)
    parser.add_argument("--output", type=Path)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    root = resolve_repo_root(args.root)
    policy = load_policy(root, args)
    changed_files = (
        [normalize_changed_file(item) for item in args.changed_file]
        if args.changed_file
        else discover_changed_files(root, args.base_ref, args.head_ref)
    )
    evidence_path = args.observation_evidence
    if evidence_path is not None and not evidence_path.is_absolute():
        evidence_path = root / evidence_path
    evidence = _read_evidence(evidence_path)
    plan = build_plan(
        policy,
        changed_files,
        base_ref=args.base_ref,
        head_ref=args.head_ref,
        mode=args.mode,
        evidence=evidence,
    )
    rendered = json.dumps(plan, ensure_ascii=False, indent=2, sort_keys=True) + "\n"
    if args.output is not None:
        output = (args.output if args.output.is_absolute() else root / args.output).resolve()
        if not output.is_relative_to(policy.binary_root):
            raise GateInputError(
                f"validation plan output must stay within inventory binaryRoot: {policy.binary_root}"
            )
        output.parent.mkdir(parents=True, exist_ok=True)
        temporary = output.with_name(f".{output.name}.tmp")
        temporary.write_text(rendered, encoding="utf-8")
        temporary.replace(output)
        print(f"Validation selection plan written to {output}")
    else:
        sys.stdout.write(rendered)
    return 0


if __name__ == "__main__":
    raise SystemExit(run_gate(main))
