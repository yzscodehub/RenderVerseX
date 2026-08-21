#!/usr/bin/env python3
"""Enforce the development-only, pinned, real-C99 FBX import contract."""
from __future__ import annotations

import argparse
import json
import re
from pathlib import Path

from architecture_gate_common import resolve_repo_root, run_gate


UFBX_VERSION = "0.14.3"
UFBX_COMMIT = "19bdb7e7ef02eb914d5e7211a3685f50ee6d27e3"
UFBX_SHA512 = (
    "66d42b62ed3e55c19cb99cd55876853bf48ce3b09b026fa54d1719e78b40ccdf"
    "d2ca44d3410fb9552f60e6d5cdf9e5d911ee4164d905250af87f90011dbab1a4"
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", default=".", help="Repository root.")
    return parser.parse_args()


def read_required(root: Path, relative: str, failures: list[str]) -> str:
    path = root / relative
    if not path.is_file():
        failures.append(f"required FBX contract input is missing: {relative}")
        return ""
    return path.read_text(encoding="utf-8", errors="replace")


def load_json(root: Path, relative: str, failures: list[str]) -> dict:
    text = read_required(root, relative, failures)
    if not text:
        return {}
    try:
        value = json.loads(text)
    except json.JSONDecodeError as error:
        failures.append(f"{relative} is invalid JSON: {error}")
        return {}
    if not isinstance(value, dict):
        failures.append(f"{relative} must contain a JSON object")
        return {}
    return value


def validate_port(root: Path, failures: list[str]) -> None:
    manifest = load_json(root, "vcpkg-ports/ufbx/vcpkg.json", failures)
    if manifest.get("name") != "ufbx" or manifest.get("version") != UFBX_VERSION:
        failures.append(f"ufbx overlay must declare immutable version {UFBX_VERSION}")

    portfile = read_required(root, "vcpkg-ports/ufbx/portfile.cmake", failures)
    commit = re.search(r"\bREF\s+([0-9a-fA-F]{40})\b", portfile)
    digest = re.search(r"\bSHA512\s+([0-9a-fA-F]{128})\b", portfile)
    if not commit or commit.group(1).lower() != UFBX_COMMIT:
        failures.append("ufbx overlay REF must be the approved immutable v0.14.3 commit")
    if not digest or digest.group(1).lower() != UFBX_SHA512:
        failures.append("ufbx overlay SHA512 must match the approved v0.14.3 archive")

    cmake = read_required(root, "vcpkg-ports/ufbx/CMakeLists.txt", failures)
    normalized = re.sub(r"\s+", " ", cmake)
    required_patterns = (
        (r"project\s*\([^)]*\bLANGUAGES\s+C\b[^)]*\)", "C-only project"),
        (r"add_library\s*\(\s*ufbx\s+STATIC\s+ufbx\.c\s*\)", "real static C source"),
        (r"add_library\s*\(\s*ufbx::ufbx\s+ALIAS\s+ufbx\s*\)", "ufbx::ufbx target"),
        (r"\bc_std_99\b", "C99 compile feature"),
        (r"\bC_STANDARD_REQUIRED\s+YES\b", "required C99 standard"),
    )
    for pattern, description in required_patterns:
        if re.search(pattern, normalized, re.IGNORECASE) is None:
            failures.append(f"ufbx overlay must define a {description}")
    if re.search(r"\bLANGUAGES\s+[^)]*CXX", normalized, re.IGNORECASE):
        failures.append("ufbx overlay must not classify the library as C++")
    if "HEADER_FILE_ONLY" in cmake:
        failures.append("ufbx.c must compile; it cannot be a header-only/None item")


def validate_feature_matrix(root: Path, failures: list[str]) -> None:
    manifest = load_json(root, "vcpkg.json", failures)
    fbx_feature = manifest.get("features", {}).get("fbx", {})
    dependencies = fbx_feature.get("dependencies", [])
    if "ufbx" not in dependencies:
        failures.append("vcpkg fbx feature must depend on the ufbx overlay")

    presets = load_json(root, "CMakePresets.json", failures)
    configure = {
        preset.get("name"): preset
        for preset in presets.get("configurePresets", [])
        if isinstance(preset, dict) and isinstance(preset.get("name"), str)
    }
    for name in ("win_x64_base", "linux_x64_base", "mac_arm64_base"):
        variables = configure.get(name, {}).get("cacheVariables", {})
        if variables.get("RVX_ENABLE_FBX") != "ON":
            failures.append(f"developer preset {name} must enable RVX_ENABLE_FBX")
        if variables.get("VCPKG_MANIFEST_FEATURES") != "fbx":
            failures.append(f"developer preset {name} must select only the fbx feature")

    shipping_names = {
        f"{platform}_{kind}"
        for platform in ("win_x64", "linux_x64", "mac_arm64")
        for kind in ("shipping_host_release", "shipping_release")
    }
    for name in sorted(shipping_names):
        variables = configure.get(name, {}).get("cacheVariables", {})
        if variables.get("RVX_ENABLE_FBX") != "OFF":
            failures.append(f"shipping preset {name} must disable RVX_ENABLE_FBX")
        if variables.get("VCPKG_MANIFEST_FEATURES") != "":
            failures.append(f"shipping preset {name} must exclude the fbx feature")


def validate_integration(root: Path, failures: list[str]) -> None:
    root_cmake = read_required(root, "CMakeLists.txt", failures)
    asset_import_cmake = read_required(
        root, "Source/Developer/AssetImport/CMakeLists.txt", failures
    )
    pipeline_cmake = read_required(
        root, "Source/Developer/AssetPipeline/CMakeLists.txt", failures
    )
    pipeline_source = read_required(
        root, "Source/Developer/AssetPipeline/Private/AssetPipeline.cpp", failures
    )
    public_header = read_required(
        root, "Source/Developer/AssetImport/Include/AssetImport/FBXImporter.h", failures
    )

    if "option(RVX_ENABLE_FBX" not in root_cmake:
        failures.append("root CMake must expose RVX_ENABLE_FBX")
    compact_import = re.sub(r"\s+", " ", asset_import_cmake)
    for token, diagnostic in (
        ("find_package(ufbx CONFIG REQUIRED)", "required ufbx package lookup"),
        ("ufbx::ufbx", "ufbx link target"),
        ("RVX_ENABLE_FBX", "feature guard"),
        ("Private/FBXImporter.cpp", "real FBX importer source"),
    ):
        if token not in compact_import:
            failures.append(f"AssetImport CMake is missing its {diagnostic}")
    if "RVX_ENABLE_FBX" not in pipeline_cmake:
        failures.append("AssetPipeline must receive the FBX feature contract")
    compact_pipeline = re.sub(r"\s+", " ", pipeline_source)
    if re.search(
        r"#if defined\(RVX_ENABLE_FBX\).*\{\s*\"\.fbx\"\s*,\s*AssetType::Model\s*\}",
        compact_pipeline,
    ) is None:
        failures.append(
            "AssetPipeline must classify .fbx as a model only inside the FBX feature guard"
        )
    if re.search(r"\bufbx_", public_header):
        failures.append("the public FBXImporter header must not expose ufbx types")

    scan_roots = (
        root / "CMakeLists.txt",
        root / "Source",
        root / "vcpkg-ports/ufbx",
    )
    forbidden = (
        (re.compile(r"\bFetchContent(?:_Declare|_MakeAvailable)?\b"), "FetchContent ufbx fallback"),
        (re.compile(r"\bHAS_UFBX\b"), "HAS_UFBX stub contract"),
        (re.compile(r"\bstruct\s+ufbx_"), "fake public ufbx declaration"),
    )
    for scan_root in scan_roots:
        paths = [scan_root] if scan_root.is_file() else scan_root.rglob("*")
        for path in paths:
            if not path.is_file() or path.suffix.lower() not in {
                ".cmake", ".txt", ".h", ".hpp", ".cpp", ".c", ".json"
            }:
                continue
            text = path.read_text(encoding="utf-8", errors="replace")
            for pattern, diagnostic in forbidden:
                if pattern.search(text):
                    failures.append(
                        f"{path.relative_to(root).as_posix()}: {diagnostic} is forbidden"
                    )


def main() -> int:
    root = resolve_repo_root(parse_args().root)
    failures: list[str] = []
    validate_port(root, failures)
    validate_feature_matrix(root, failures)
    validate_integration(root, failures)
    if failures:
        print("FBX import contract failures:")
        for failure in failures:
            print(f"  {failure}")
        return 1
    print("Pinned development-only FBX import contract check passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(run_gate(main))
