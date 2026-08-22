#!/usr/bin/env python3
"""Validate machine-readable engine capabilities against repository truth."""
from __future__ import annotations

import argparse
import fnmatch
import json
import re
from pathlib import Path
from typing import Any

from architecture_gate_common import resolve_repo_root, run_gate


STATUS_VALUES = {"integrated", "partial", "experimental", "deferred"}
COMPOSITION_VALUES = {"engine-required", "runtime-optional", "not-composed"}
CAPABILITY_FIELDS = {
    "id",
    "status",
    "module",
    "runtimeComposition",
    "shippingSupported",
    "requiredEvidence",
}
FEATURE_CAPABILITY_SURFACES = {
    "particle": ("particle-runtime", "particle-rendering"),
    "water": ("water-runtime", "water-rendering"),
    "terrain": ("terrain-runtime", "terrain-rendering"),
}
FEATURE_CAPABILITY_RULES = {
    "particle-runtime": {
        "integrated": "ParticleRuntimeQualification",
        "shipping": "ParticleRuntimeShippingValidation",
    },
    "particle-rendering": {
        "integrated": "ParticleRenderingQualification",
        "shipping": "ParticleRenderingShippingValidation",
    },
    "water-runtime": {
        "integrated": "WaterRuntimeQualification",
        "shipping": "WaterRuntimeShippingValidation",
    },
    "water-rendering": {
        "integrated": "WaterRenderingQualification",
        "shipping": "WaterRenderingShippingValidation",
    },
    "terrain-runtime": {
        "integrated": "TerrainRuntimeQualification",
        "shipping": "TerrainRuntimeShippingValidation",
    },
    "terrain-rendering": {
        "integrated": "TerrainRenderingQualification",
        "shipping": "TerrainRenderingShippingValidation",
    },
}
GENERIC_FEATURE_SNAPSHOT_EVIDENCE = "EcsFeatureSnapshotValidation"
RETIRED_MODEL_V1 = "RVX_MODEL_ARTIFACT_" + "V1"
RETIRED_MODEL_V2 = "RVX_MODEL_ARTIFACT_" + "V2"
RETIRED_GLTF_MODEL = "RVX_GLTF_MODEL_" + "WITH_ANIMATION_V2"
STALE_CURRENT_CONTRACTS = (
    (re.compile(r"\bResourceManager::Get\s*\("), "retired ResourceManager singleton"),
    (re.compile(r"\bPhysicsWorld::Get\s*\("), "retired PhysicsWorld singleton example"),
    (re.compile(r"\bSupportsPreparedLoading\b"), "retired prepared-loading capability probe"),
    (re.compile(rf"\b{RETIRED_MODEL_V1}\b"), "retired model artifact v1"),
    (re.compile(rf"\b{RETIRED_MODEL_V2}\b"), "retired model artifact v2"),
    (re.compile(rf"\b{RETIRED_GLTF_MODEL}\b"), "retired glTF-specific model artifact"),
    (re.compile(r"\bworld->GetSubsystem\s*<"), "nonexistent World subsystem lookup example"),
    (re.compile(r"FetchContent[\s\S]{0,200}\bufbx\b", re.IGNORECASE), "ufbx FetchContent fallback"),
    (re.compile(r"(?:build/<preset>|<absolute-build-root>)/ShippingFixture(?:/|\b)"), "obsolete Shipping fixture path"),
)


class DuplicateKeyError(ValueError):
    pass


def reject_duplicate_keys(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise DuplicateKeyError(f"duplicate key: {key}")
        result[key] = value
    return result


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", default=".", help="Repository root.")
    return parser.parse_args()


def read_required(root: Path, relative: str, failures: list[str]) -> str:
    path = root / relative
    if not path.is_file():
        failures.append(f"required capability-contract input is missing: {relative}")
        return ""
    return path.read_text(encoding="utf-8", errors="replace")


def load_json(root: Path, relative: str, failures: list[str]) -> dict[str, Any]:
    text = read_required(root, relative, failures)
    if not text:
        return {}
    try:
        value = json.loads(text, object_pairs_hook=reject_duplicate_keys)
    except (json.JSONDecodeError, DuplicateKeyError) as error:
        failures.append(f"{relative} is invalid JSON: {error}")
        return {}
    if not isinstance(value, dict):
        failures.append(f"{relative} must contain a JSON object")
        return {}
    return value


def resolve_contract_path(
    root: Path, raw_path: object, label: str, failures: list[str]
) -> Path | None:
    if not isinstance(raw_path, str) or not raw_path:
        failures.append(f"{label} must be a non-empty repository-relative path")
        return None
    candidate = Path(raw_path)
    if candidate.is_absolute() or ".." in candidate.parts:
        failures.append(f"{label} must stay inside the repository: {raw_path}")
        return None
    resolved = (root / candidate).resolve()
    try:
        resolved.relative_to(root)
    except ValueError:
        failures.append(f"{label} escapes the repository: {raw_path}")
        return None
    return resolved


def public_preset_names(presets: dict[str, Any], key: str) -> list[str]:
    values = presets.get(key, [])
    if not isinstance(values, list):
        return []
    return [
        value["name"]
        for value in values
        if isinstance(value, dict)
        and isinstance(value.get("name"), str)
        and value.get("hidden") is not True
    ]


def validate_build_contract(
    root: Path,
    contract: dict[str, Any],
    manifest: dict[str, Any],
    presets: dict[str, Any],
    vcpkg: dict[str, Any],
    failures: list[str],
) -> None:
    product = contract.get("product")
    build = contract.get("buildContract")
    if not isinstance(product, dict) or not isinstance(build, dict):
        failures.append("product and buildContract must be JSON objects")
        return

    cmake_text = read_required(root, "CMakeLists.txt", failures)
    cmake_match = re.search(
        r"cmake_minimum_required\s*\(\s*VERSION\s+([0-9]+(?:\.[0-9]+)+)",
        cmake_text,
        re.IGNORECASE,
    )
    expected_cmake = product.get("cmakeMinimum")
    if not cmake_match or cmake_match.group(1) != expected_cmake:
        failures.append(
            "product.cmakeMinimum must exactly match root cmake_minimum_required"
        )

    if vcpkg.get("description") != product.get("vcpkgDescription"):
        failures.append("product.vcpkgDescription must exactly match vcpkg.json")

    manifest_path = build.get("moduleManifest")
    if manifest_path != "Docs/module-boundaries.json":
        failures.append("buildContract.moduleManifest must name Docs/module-boundaries.json")
    if manifest.get("schema") != "RVX.ModuleBoundaryManifest" or manifest.get("schemaVersion") != 2:
        failures.append("module Manifest must use RVX.ModuleBoundaryManifest v2")

    expected_configure = build.get("configurePresets")
    expected_test = build.get("testPresets")
    actual_configure = public_preset_names(presets, "configurePresets")
    actual_test = public_preset_names(presets, "testPresets")
    if not isinstance(expected_configure, list) or expected_configure != actual_configure:
        failures.append("buildContract.configurePresets does not match public CMake configure presets")
    if not isinstance(expected_test, list) or expected_test != actual_test:
        failures.append("buildContract.testPresets does not match public CTest presets")
    if len(actual_configure) != len(set(actual_configure)) or len(actual_test) != len(set(actual_test)):
        failures.append("public preset names must be unique")

    artifacts = build.get("artifactContracts")
    if not isinstance(artifacts, list) or not artifacts:
        failures.append("buildContract.artifactContracts must be a non-empty array")
    else:
        artifact_ids: set[str] = set()
        for index, artifact in enumerate(artifacts):
            label = f"artifactContracts[{index}]"
            if not isinstance(artifact, dict):
                failures.append(f"{label} must be an object")
                continue
            artifact_id = artifact.get("id")
            magic = artifact.get("magic")
            if not isinstance(artifact_id, str) or not artifact_id or artifact_id in artifact_ids:
                failures.append(f"{label}.id must be unique and non-empty")
            else:
                artifact_ids.add(artifact_id)
            if not isinstance(magic, str) or not magic:
                failures.append(f"{label}.magic must be non-empty")
                continue
            source = resolve_contract_path(root, artifact.get("source"), f"{label}.source", failures)
            if source is None or not source.is_file():
                failures.append(f"{label}.source does not exist")
            elif magic not in source.read_text(encoding="utf-8", errors="replace"):
                failures.append(f"{label}.magic is absent from its declared source")

    render_graph = build.get("renderGraph")
    if not isinstance(render_graph, dict):
        failures.append("buildContract.renderGraph must be an object")
    else:
        source = resolve_contract_path(
            root, render_graph.get("source"), "buildContract.renderGraph.source", failures
        )
        text = "" if source is None or not source.is_file() else source.read_text(
            encoding="utf-8", errors="replace"
        )
        target = render_graph.get("target")
        alias = render_graph.get("alias")
        module = render_graph.get("manifestModule")
        if not isinstance(target, str) or re.search(
            rf"add_library\s*\(\s*{re.escape(target)}\s+STATIC\b", text
        ) is None:
            failures.append("RenderGraph contract must name its real static target")
        if not isinstance(alias, str) or not isinstance(target, str) or re.search(
            rf"add_library\s*\(\s*{re.escape(alias)}\s+ALIAS\s+{re.escape(target)}\s*\)",
            text,
        ) is None:
            failures.append("RenderGraph contract must name its real public alias")
        registration = re.search(
            rf"rvx_register_module_target\s*\([^)]*\bTARGET\s+{re.escape(str(target))}\b[^)]*\bMODULE\s+{re.escape(str(module))}\b[^)]*\)",
            text,
            re.DOTALL,
        )
        if registration is None:
            failures.append("RenderGraph target must be registered inside its declared Manifest module")

    fbx = build.get("fbx")
    features = vcpkg.get("features", {})
    if not isinstance(fbx, dict) or fbx.get("shippingEnabled") is not False:
        failures.append("buildContract.fbx must explicitly disable Shipping support")
    elif not isinstance(features, dict) or fbx.get("vcpkgFeature") not in features:
        failures.append("buildContract.fbx.vcpkgFeature is absent from vcpkg.json")


def module_map(manifest: dict[str, Any], failures: list[str]) -> dict[str, dict[str, Any]]:
    modules = manifest.get("modules")
    if not isinstance(modules, list) or not modules:
        failures.append("module Manifest modules must be a non-empty array")
        return {}
    result: dict[str, dict[str, Any]] = {}
    for index, module in enumerate(modules):
        if not isinstance(module, dict) or not isinstance(module.get("id"), str):
            failures.append(f"module Manifest entry {index} has no string id")
            continue
        module_id = module["id"]
        if module_id in result:
            failures.append(f"duplicate module Manifest id: {module_id}")
        result[module_id] = module
    return result


def validate_evidence(
    root: Path, capability_id: str, evidence: object, failures: list[str]
) -> None:
    if not isinstance(evidence, list):
        failures.append(f"capability {capability_id} requiredEvidence must be an array")
        return
    for index, item in enumerate(evidence):
        label = f"capability {capability_id} requiredEvidence[{index}]"
        if not isinstance(item, dict) or set(item) != {"path", "contains"}:
            failures.append(f"{label} must contain only path and contains")
            continue
        token = item.get("contains")
        if not isinstance(token, str) or not token:
            failures.append(f"{label}.contains must be non-empty")
            continue
        path = resolve_contract_path(root, item.get("path"), f"{label}.path", failures)
        if path is None or not path.is_file():
            failures.append(f"{label}.path does not exist")
        elif token not in path.read_text(encoding="utf-8", errors="replace"):
            failures.append(f"{label} token is absent from its evidence source: {token}")


def evidence_uses_generic_feature_snapshot(evidence: object) -> bool:
    if not isinstance(evidence, list):
        return False
    for item in evidence:
        if not isinstance(item, dict):
            continue
        path = item.get("path")
        token = item.get("contains")
        if (
            isinstance(path, str)
            and GENERIC_FEATURE_SNAPSHOT_EVIDENCE in path.replace("\\", "/")
        ) or (
            isinstance(token, str)
            and GENERIC_FEATURE_SNAPSHOT_EVIDENCE in token
        ):
            return True
    return False


def has_dedicated_test_evidence(evidence: object, token: str) -> bool:
    if not isinstance(evidence, list):
        return False
    for item in evidence:
        if not isinstance(item, dict):
            continue
        path = item.get("path")
        contains = item.get("contains")
        if (
            isinstance(path, str)
            and path.replace("\\", "/").startswith("Tests/")
            and isinstance(contains, str)
            and token in contains
        ):
            return True
    return False


def validate_capabilities(
    root: Path,
    contract: dict[str, Any],
    modules: dict[str, dict[str, Any]],
    failures: list[str],
) -> None:
    capabilities = contract.get("capabilities")
    if not isinstance(capabilities, list) or not capabilities:
        failures.append("capabilities must be a non-empty array")
        return
    engine_reachable = {"Engine"}
    pending = ["Engine"]
    while pending:
        current = pending.pop()
        for allowed in modules.get(current, {}).get("allowedModules", []):
            if allowed in modules and allowed not in engine_reachable:
                engine_reachable.add(allowed)
                pending.append(allowed)
    capability_ids: set[str] = set()
    seen_statuses: set[str] = set()
    for index, capability in enumerate(capabilities):
        label = f"capabilities[{index}]"
        if not isinstance(capability, dict):
            failures.append(f"{label} must be an object")
            continue
        if set(capability) != CAPABILITY_FIELDS:
            failures.append(f"{label} must contain exactly {sorted(CAPABILITY_FIELDS)}")
            continue
        capability_id = capability.get("id")
        status = capability.get("status")
        module_id = capability.get("module")
        composition = capability.get("runtimeComposition")
        shipping = capability.get("shippingSupported")
        evidence = capability.get("requiredEvidence")
        if not isinstance(capability_id, str) or not re.fullmatch(r"[a-z0-9]+(?:-[a-z0-9]+)*", capability_id):
            failures.append(f"{label}.id must use lowercase kebab-case")
            capability_id = label
        elif capability_id in capability_ids:
            failures.append(f"duplicate capability id: {capability_id}")
        else:
            capability_ids.add(capability_id)
        if capability_id in FEATURE_CAPABILITY_SURFACES:
            failures.append(
                f"ambiguous broad capability id is forbidden: {capability_id}; "
                "declare separate runtime and rendering capabilities"
            )
        if status not in STATUS_VALUES:
            failures.append(f"capability {capability_id} has invalid status: {status}")
        else:
            seen_statuses.add(status)
        if module_id not in modules:
            failures.append(f"capability {capability_id} names unknown module: {module_id}")
            module = {}
        else:
            module = modules[module_id]
        if composition not in COMPOSITION_VALUES:
            failures.append(f"capability {capability_id} has invalid runtimeComposition")
        if not isinstance(shipping, bool):
            failures.append(f"capability {capability_id} shippingSupported must be boolean")
        elif shipping and not module.get("shippingAllowed"):
            failures.append(f"capability {capability_id} cannot claim Shipping support")
        if shipping is True and composition == "not-composed":
            failures.append(f"capability {capability_id} cannot ship while not composed")
        if composition == "engine-required" and module_id not in engine_reachable:
            failures.append(f"capability {capability_id} is not admitted by Engine's module policy")
        if status == "integrated" and composition != "engine-required":
            failures.append(f"integrated capability {capability_id} must be engine-required")
        if status == "experimental" and composition != "not-composed":
            failures.append(f"experimental capability {capability_id} must remain not-composed")
        if status == "deferred":
            if composition != "not-composed" or shipping is not False:
                failures.append(f"deferred capability {capability_id} must be uncomposed and non-Shipping")
            if evidence != []:
                failures.append(f"deferred capability {capability_id} must not claim required evidence")
        elif evidence == []:
            failures.append(f"non-deferred capability {capability_id} requires evidence")
        validate_evidence(root, capability_id, evidence, failures)

        feature_rule = FEATURE_CAPABILITY_RULES.get(capability_id)
        if feature_rule is not None:
            if evidence_uses_generic_feature_snapshot(evidence):
                failures.append(
                    f"capability {capability_id} cannot use generic "
                    f"{GENERIC_FEATURE_SNAPSHOT_EVIDENCE} transport evidence"
                )
            if status == "integrated" and not has_dedicated_test_evidence(
                evidence, feature_rule["integrated"]
            ):
                failures.append(
                    f"integrated capability {capability_id} requires dedicated "
                    f"test evidence containing {feature_rule['integrated']}"
                )
            if shipping is True and not has_dedicated_test_evidence(
                evidence, feature_rule["shipping"]
            ):
                failures.append(
                    f"Shipping capability {capability_id} requires dedicated "
                    f"test evidence containing {feature_rule['shipping']}"
                )

    required_feature_ids = {
        capability_id
        for surfaces in FEATURE_CAPABILITY_SURFACES.values()
        for capability_id in surfaces
    }
    missing_feature_ids = sorted(required_feature_ids - capability_ids)
    if missing_feature_ids:
        failures.append(
            "capability contract is missing required runtime/rendering feature "
            f"surfaces: {', '.join(missing_feature_ids)}"
        )
    if seen_statuses != STATUS_VALUES:
        failures.append("capability contract must represent every supported status")


def is_below(path: str, root_path: str) -> bool:
    normalized_root = root_path.rstrip("/")
    return path == normalized_root or path.startswith(normalized_root + "/")


def validate_documentation(
    root: Path, contract: dict[str, Any], failures: list[str]
) -> None:
    classes = contract.get("documentationClasses")
    if not isinstance(classes, dict):
        failures.append("documentationClasses must be an object")
        return
    precedence = classes.get("classificationPrecedence")
    expected_precedence = [
        "currentContracts",
        "futureProposalRoots",
        "historicalEvidenceRoots",
        "historicalEvidenceGlobs",
    ]
    if precedence != expected_precedence:
        failures.append("documentation classification precedence is invalid")
    current = classes.get("currentContracts")
    future_roots = classes.get("futureProposalRoots")
    history_roots = classes.get("historicalEvidenceRoots")
    history_globs = classes.get("historicalEvidenceGlobs")
    if not all(isinstance(value, list) for value in (current, future_roots, history_roots, history_globs)):
        failures.append("documentation class entries must all be arrays")
        return
    current_set: set[str] = set()
    for index, raw_path in enumerate(current):
        path = resolve_contract_path(root, raw_path, f"currentContracts[{index}]", failures)
        if path is None or not path.is_file():
            failures.append(f"current contract does not exist: {raw_path}")
        elif raw_path in current_set:
            failures.append(f"duplicate current contract: {raw_path}")
        else:
            current_set.add(raw_path)
    for class_name, roots in (("futureProposalRoots", future_roots), ("historicalEvidenceRoots", history_roots)):
        for index, raw_path in enumerate(roots):
            path = resolve_contract_path(root, raw_path, f"{class_name}[{index}]", failures)
            if path is None or not path.is_dir():
                failures.append(f"documentation class root does not exist: {raw_path}")
    if not all(isinstance(pattern, str) and pattern for pattern in history_globs):
        failures.append("historicalEvidenceGlobs entries must be non-empty strings")

    docs_root = root / "Docs"
    if docs_root.is_dir():
        for path in sorted(docs_root.rglob("*")):
            if not path.is_file() or path.suffix.lower() not in {".md", ".json"}:
                continue
            relative = path.relative_to(root).as_posix()
            classified = (
                relative in current_set
                or any(is_below(relative, value) for value in future_roots if isinstance(value, str))
                or any(is_below(relative, value) for value in history_roots if isinstance(value, str))
                or any(fnmatch.fnmatchcase(relative, pattern) for pattern in history_globs if isinstance(pattern, str))
            )
            if not classified:
                failures.append(f"documentation file has no current/future/history class: {relative}")

    stale_paths: list[Path] = []
    for raw_path in current_set:
        path = root / raw_path
        if path.suffix.lower() == ".md":
            stale_paths.append(path)
    for include_root in root.glob("Source/*/*/Include"):
        stale_paths.extend(
            path for path in include_root.rglob("*")
            if path.is_file() and path.suffix.lower() in {".h", ".hpp"}
        )
    for path in sorted(set(stale_paths)):
        text = path.read_text(encoding="utf-8", errors="replace")
        for pattern, diagnostic in STALE_CURRENT_CONTRACTS:
            if pattern.search(text):
                failures.append(f"{path.relative_to(root).as_posix()}: {diagnostic}")


def main() -> int:
    root = resolve_repo_root(parse_args().root)
    failures: list[str] = []
    contract = load_json(root, "Docs/engine-capabilities.json", failures)
    manifest = load_json(root, "Docs/module-boundaries.json", failures)
    presets = load_json(root, "CMakePresets.json", failures)
    vcpkg = load_json(root, "vcpkg.json", failures)
    if contract.get("schema") != "RVX.EngineCapabilities" or contract.get("schemaVersion") != 1:
        failures.append("engine capability contract must use RVX.EngineCapabilities v1")
    modules = module_map(manifest, failures)
    validate_build_contract(root, contract, manifest, presets, vcpkg, failures)
    validate_capabilities(root, contract, modules, failures)
    validate_documentation(root, contract, failures)
    if failures:
        print("Engine capability contract failures:")
        for failure in failures:
            print(f"  {failure}")
        return 1
    print(
        f"Engine capability contract passed: {len(contract.get('capabilities', []))} "
        f"capabilities, {len(modules)} Manifest modules."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(run_gate(main))
