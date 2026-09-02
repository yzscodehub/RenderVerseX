#!/usr/bin/env python3
"""Fail-closed semantic validation for the required CI workflow topology."""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path


WORKFLOWS = {
    "ci.yml": {
        "architecture",
        "windows-debug",
        "linux-debug",
        "linux-tsan",
        "macos-arm64",
        "shipping",
        "raster-correctness",
        "windows-gfx24-hardware",
        "linux-gfx24-vulkan-rt",
        "pr-verdict",
    },
    "nightly.yml": {
        "windows-qualification",
        "linux-full",
        "linux-tsan-full",
        "macos-full",
    },
    "weekly.yml": {"soak"},
}

PR_SMOKE_PRESETS = {
    "win_x64_pr_debug": "win_x64_pr_debug_smoke",
    "linux_x64_pr_debug": "linux_x64_pr_debug_smoke",
    "mac_arm64_pr_debug": "mac_arm64_pr_debug_smoke",
}

SHIPPING_PRESETS = {
    "win_x64_shipping_release": "win_x64_shipping_release_shipping",
    "linux_x64_shipping_release": "linux_x64_shipping_release_shipping",
    "mac_arm64_shipping_release": "mac_arm64_shipping_release_shipping",
}

COMPATIBILITY_PRESETS = {
    "win_x64_nightly_compatibility_debug",
    "linux_x64_nightly_compatibility_debug",
}

WEEKLY_PRESETS = {
    "win_x64_weekly_soak_debug",
    "linux_x64_weekly_soak_debug",
    "mac_arm64_weekly_soak_debug",
}

GFX15_CONFIGURE_PRESET = "win_x64_qualification_debug"
GFX15_TEST_PRESET = "win_x64_qualification_debug_gfx15_translucency"
GFX15_BUILD_TARGET = "RVXValidationSuiteGfx15Translucency"
GFX15_LABEL = "suite.gfx15-translucency"
GFX15_REQUIRED_EVIDENCE = {
    "Tests/VisualArtifacts/Debug/RenderVerseSamples/RenderVerseSamplesDX12GFX15ForwardPlusTranslucency_640x360.ppm",
    "Tests/VisualArtifacts/Debug/RenderVerseSamples/RenderVerseSamplesDX12GFX15DeferredTranslucency_640x360.ppm",
    "Tests/VisualArtifacts/Debug/RenderVerseSamples/RenderVerseSamplesVulkanGFX15ForwardPlusTranslucency_640x360.ppm",
    "Tests/VisualArtifacts/Debug/RenderVerseSamples/RenderVerseSamplesVulkanGFX15DeferredTranslucency_640x360.ppm",
    "Tests/SampleReports/Debug/RenderVerseSamples/RenderVerseSamplesDX12GFX15ForwardPlusTranslucency_640x360.json",
    "Tests/SampleReports/Debug/RenderVerseSamples/RenderVerseSamplesDX12GFX15DeferredTranslucency_640x360.json",
    "Tests/SampleReports/Debug/RenderVerseSamples/RenderVerseSamplesVulkanGFX15ForwardPlusTranslucency_640x360.json",
    "Tests/SampleReports/Debug/RenderVerseSamples/RenderVerseSamplesVulkanGFX15DeferredTranslucency_640x360.json",
    "Tests/StartupTimelines/Debug/RenderVerseSamples/RenderVerseSamplesDX12GFX15ForwardPlusTranslucency_640x360.json",
    "Tests/StartupTimelines/Debug/RenderVerseSamples/RenderVerseSamplesDX12GFX15DeferredTranslucency_640x360.json",
    "Tests/StartupTimelines/Debug/RenderVerseSamples/RenderVerseSamplesVulkanGFX15ForwardPlusTranslucency_640x360.json",
    "Tests/StartupTimelines/Debug/RenderVerseSamples/RenderVerseSamplesVulkanGFX15DeferredTranslucency_640x360.json",
}

GFX18_CONFIGURE_PRESET = "win_x64_qualification_debug"
GFX18_TEST_PRESET = "win_x64_qualification_debug_gfx18_water"
GFX18_BUILD_TARGET = "RVXValidationSuiteGfx18Water"
GFX18_LABEL = "suite.gfx18-water"
GFX18_REQUIRED_EVIDENCE = {
    "Tests/VisualArtifacts/Debug/RenderVerseSamples/RenderVerseSamplesDX12GFX18ForwardPlusWaterSurface_640x360.ppm",
    "Tests/VisualArtifacts/Debug/RenderVerseSamples/RenderVerseSamplesDX12GFX18DeferredWaterSurface_640x360.ppm",
    "Tests/VisualArtifacts/Debug/RenderVerseSamples/RenderVerseSamplesVulkanGFX18ForwardPlusWaterSurface_640x360.ppm",
    "Tests/VisualArtifacts/Debug/RenderVerseSamples/RenderVerseSamplesVulkanGFX18DeferredWaterSurface_640x360.ppm",
    "Tests/SampleReports/Debug/RenderVerseSamples/RenderVerseSamplesDX12GFX18ForwardPlusWaterSurface_640x360.json",
    "Tests/SampleReports/Debug/RenderVerseSamples/RenderVerseSamplesDX12GFX18DeferredWaterSurface_640x360.json",
    "Tests/SampleReports/Debug/RenderVerseSamples/RenderVerseSamplesVulkanGFX18ForwardPlusWaterSurface_640x360.json",
    "Tests/SampleReports/Debug/RenderVerseSamples/RenderVerseSamplesVulkanGFX18DeferredWaterSurface_640x360.json",
    "Tests/StartupTimelines/Debug/RenderVerseSamples/RenderVerseSamplesDX12GFX18ForwardPlusWaterSurface_640x360.json",
    "Tests/StartupTimelines/Debug/RenderVerseSamples/RenderVerseSamplesDX12GFX18DeferredWaterSurface_640x360.json",
    "Tests/StartupTimelines/Debug/RenderVerseSamples/RenderVerseSamplesVulkanGFX18ForwardPlusWaterSurface_640x360.json",
    "Tests/StartupTimelines/Debug/RenderVerseSamples/RenderVerseSamplesVulkanGFX18DeferredWaterSurface_640x360.json",
}

# The hermetic GFX24 contract suite stays independent from native candidate
# producers.  Candidate artifacts have no human-approved baseline and must
# never be mistaken for an automatic visual/performance pass.
GFX24_CONFIGURE_PRESET = "win_x64_debug"
GFX24_BUILD_PRESET = "win_x64_debug"
GFX24_TEST_PRESET = "win_x64_debug_gfx24_qualification"
GFX24_LABEL = "suite.gfx24-qualification"
GFX24_CANDIDATE_CONFIGURE_PRESET = "win_x64_gfx24_candidate_release"
GFX24_CANDIDATE_BUILD_PRESET = "win_x64_gfx24_candidate_release"
GFX24_CANDIDATE_TEST_BASE = "win_x64_gfx24_candidate_release_lane_base"
GFX24_CANDIDATE_TEST_PRESET = "win_x64_gfx24_candidate_release_gfx24_candidate"
GFX24_CANDIDATE_BUILD_TARGET = "RVXValidationSuiteGfx24Candidate"
GFX24_CANDIDATE_LABEL = "suite.gfx24-candidate"

GFX24_RASTER_BUILD_TARGET = "RVXValidationSuiteGfx24RasterCorrectness"
GFX24_RASTER_LABEL = "suite.gfx24-raster-correctness"
GFX24_RASTER_CONTRACTS = {
    "Windows": {
        "backend": "DX12",
        "configure": "win_x64_gfx24_raster_release",
        "build_dir": "build/win_x64_gfx24_raster_release",
        "test": "win_x64_gfx24_raster_release_run",
        "evidence": (
            "Tests/Gfx24RasterCorrectness/Release/DX12/ForwardPlus/series.json",
            "Tests/Gfx24RasterCorrectness/Release/DX12/ForwardPlus/series-validation.json",
            "Tests/Gfx24RasterCorrectness/Release/DX12/Deferred/series.json",
            "Tests/Gfx24RasterCorrectness/Release/DX12/Deferred/series-validation.json",
        ),
    },
    "Linux": {
        "backend": "Vulkan",
        "configure": "linux_x64_gfx24_raster_release",
        "build_dir": "build/linux_x64_gfx24_raster_release",
        "test": "linux_x64_gfx24_raster_release_run",
        "evidence": (
            "Tests/Gfx24RasterCorrectness/Release/Vulkan/ForwardPlus/series.json",
            "Tests/Gfx24RasterCorrectness/Release/Vulkan/ForwardPlus/series-validation.json",
            "Tests/Gfx24RasterCorrectness/Release/Vulkan/Deferred/series.json",
            "Tests/Gfx24RasterCorrectness/Release/Vulkan/Deferred/series-validation.json",
        ),
    },
    "macOS": {
        "backend": "Metal",
        "configure": "macos_arm64_gfx24_raster_release",
        "build_dir": "build/macos_arm64_gfx24_raster_release",
        "test": "macos_arm64_gfx24_raster_release_run",
        "evidence": (
            "Tests/Gfx24RasterCorrectness/Release/Metal/ForwardPlus/series.json",
            "Tests/Gfx24RasterCorrectness/Release/Metal/ForwardPlus/series-validation.json",
            "Tests/Gfx24RasterCorrectness/Release/Metal/Deferred/series.json",
            "Tests/Gfx24RasterCorrectness/Release/Metal/Deferred/series-validation.json",
        ),
    },
}

GFX24_HARDWARE_RT_CONTRACTS = {
    "windows-gfx24-hardware": {
        "runner": "[self-hosted, Windows, X64, rvx-gpu-dx12-perf, rtx4070ti]",
        "configure": "win_x64_gfx24_hardware_rt_release",
        "build_dir": "build/win_x64_gfx24_hardware_rt_release",
        "test": "win_x64_gfx24_hardware_rt_release_run",
        "target": "RVXValidationSuiteGfx24HardwareRtDx12",
        "label": "suite.gfx24-hardware-rt-dx12",
        "count": "3",
        "evidence": (
            "Tests/HybridRayTracingValidation/dx12-hybrid-ray-tracing-evidence.json",
        ),
    },
    "linux-gfx24-vulkan-rt": {
        "runner": "[self-hosted, Linux, X64, rvx-gpu-vulkan-rt, rtx4070ti]",
        "configure": "linux_x64_gfx24_hardware_rt_release",
        "build_dir": "build/linux_x64_gfx24_hardware_rt_release",
        "test": "linux_x64_gfx24_hardware_rt_release_run",
        "target": "RVXValidationSuiteGfx24HardwareRtVulkan",
        "label": "suite.gfx24-hardware-rt-vulkan",
        "count": "6",
        "evidence": (
            "Tests/VulkanRayTracingValidation/vulkan-ray-tracing-evidence.json",
            "Tests/HybridRayTracingValidation/vulkan-hybrid-ray-tracing-evidence.json",
        ),
    },
}


def _read(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8")
    except OSError as error:
        raise ValueError(f"cannot read required file {path}: {error}") from error


def _job_blocks(text: str) -> dict[str, str]:
    lines = text.splitlines()
    try:
        jobs_line = next(index for index, line in enumerate(lines) if line == "jobs:")
    except StopIteration as error:
        raise ValueError("workflow has no top-level jobs mapping") from error

    starts: list[tuple[str, int]] = []
    for index in range(jobs_line + 1, len(lines)):
        match = re.fullmatch(r"  ([a-z0-9][a-z0-9-]*):", lines[index])
        if match:
            starts.append((match.group(1), index))
    blocks: dict[str, str] = {}
    for position, (name, start) in enumerate(starts):
        if name in blocks:
            raise ValueError(f"duplicate job id {name}")
        end = starts[position + 1][1] if position + 1 < len(starts) else len(lines)
        blocks[name] = "\n".join(lines[start:end])
    return blocks


def _preset_names(data: dict[str, object], category: str) -> set[str]:
    entries = data.get(category)
    if not isinstance(entries, list):
        raise ValueError(f"CMakePresets.json has no {category} array")
    names: set[str] = set()
    for entry in entries:
        if not isinstance(entry, dict) or not isinstance(entry.get("name"), str):
            raise ValueError(f"{category} contains an entry without a string name")
        name = entry["name"]
        if name in names:
            raise ValueError(f"duplicate {category} name {name}")
        names.add(name)
    return names


def validate(root: Path) -> list[str]:
    findings: list[str] = []
    workflow_root = root / ".github" / "workflows"
    workflow_text: dict[str, str] = {}
    workflow_jobs: dict[str, dict[str, str]] = {}

    for filename, required_jobs in WORKFLOWS.items():
        path = workflow_root / filename
        try:
            text = _read(path)
            jobs = _job_blocks(text)
        except ValueError as error:
            findings.append(f"{filename}: {error}")
            continue
        workflow_text[filename] = text
        workflow_jobs[filename] = jobs
        actual_jobs = set(jobs)
        if actual_jobs != required_jobs:
            findings.append(
                f"{filename}: jobs must be exactly {sorted(required_jobs)}, got {sorted(actual_jobs)}"
            )
        if "\t" in text:
            findings.append(f"{filename}: tabs are forbidden in workflow YAML")
        if text.count("${{") != text.count("}}"):
            findings.append(f"{filename}: unbalanced GitHub expression delimiters")
        for forbidden in ("continue-on-error:", "|| true", "--no-tests=ignore"):
            if forbidden in text:
                findings.append(f"{filename}: fail-open workflow token is forbidden: {forbidden}")
        if re.search(r"\brun:\s*(?:xvfb-run\s+-a\s+)?ctest\b", text):
            findings.append(
                f"{filename}: required suites must use Scripts/run_required_suite.ps1, not raw ctest"
            )
        if "actions/checkout@v4" not in text or "actions/cache@v4" not in text:
            findings.append(f"{filename}: checkout@v4 and cache@v4 are required")

    try:
        presets = json.loads(_read(root / "CMakePresets.json"))
        configure_names = _preset_names(presets, "configurePresets")
        build_names = _preset_names(presets, "buildPresets")
        test_names = _preset_names(presets, "testPresets")
    except (ValueError, json.JSONDecodeError) as error:
        findings.append(f"CMakePresets.json: {error}")
        configure_names = set()
        build_names = set()
        test_names = set()

    try:
        sample_manifest = json.loads(_read(
            root / "Samples" / "RenderVerseSamples" / "Manifests"
            / "renderverse-samples.json"
        ))
    except (ValueError, json.JSONDecodeError) as error:
        findings.append(f"RenderVerseSamples manifest: {error}")
        sample_manifest = {}

    if (
        GFX15_CONFIGURE_PRESET not in configure_names
        or GFX15_CONFIGURE_PRESET not in build_names
        or GFX15_TEST_PRESET not in test_names
    ):
        findings.append(
            "GFX15 translucency preset triplet is incomplete: "
            f"{GFX15_CONFIGURE_PRESET}/{GFX15_TEST_PRESET}"
        )
    else:
        gfx15_test_preset = next(
            entry
            for entry in presets["testPresets"]
            if entry["name"] == GFX15_TEST_PRESET
        )
        filter_object = gfx15_test_preset.get("filter")
        expected_filter = {"include": {"label": "^suite\\.gfx15-translucency$"}}
        if filter_object != expected_filter:
            findings.append(
                "GFX15 translucency test preset must select exactly "
                "suite.gfx15-translucency"
            )

    if (
        GFX18_CONFIGURE_PRESET not in configure_names
        or GFX18_CONFIGURE_PRESET not in build_names
        or GFX18_TEST_PRESET not in test_names
    ):
        findings.append(
            "GFX18 water preset triplet is incomplete: "
            f"{GFX18_CONFIGURE_PRESET}/{GFX18_TEST_PRESET}"
        )
    else:
        gfx18_test_preset = next(
            entry
            for entry in presets["testPresets"]
            if entry["name"] == GFX18_TEST_PRESET
        )
        filter_object = gfx18_test_preset.get("filter")
        expected_filter = {"include": {"label": "^suite\\.gfx18-water$"}}
        if filter_object != expected_filter:
            findings.append(
                "GFX18 water test preset must select exactly suite.gfx18-water"
            )

    if (
        GFX24_CONFIGURE_PRESET not in configure_names
        or GFX24_BUILD_PRESET not in build_names
        or GFX24_TEST_PRESET not in test_names
    ):
        findings.append(
            "GFX24 contract qualification preset triplet is incomplete: "
            f"{GFX24_CONFIGURE_PRESET}/{GFX24_TEST_PRESET}"
        )
    else:
        gfx24_configure_preset = next(
            entry
            for entry in presets["configurePresets"]
            if entry["name"] == GFX24_CONFIGURE_PRESET
        )
        cache_variables = gfx24_configure_preset.get("cacheVariables")
        if not isinstance(cache_variables, dict) or cache_variables.get(
            "RVX_ENABLE_GFX24_QUALIFICATION"
        ) != "ON":
            findings.append(
                "GFX24 contract qualification must be enabled in the shared "
                "Windows Debug preset"
            )
        gfx24_test_preset = next(
            entry
            for entry in presets["testPresets"]
            if entry["name"] == GFX24_TEST_PRESET
        )
        expected_filter = {"include": {"label": "^suite\\.gfx24-qualification$"}}
        if gfx24_test_preset.get("filter") != expected_filter:
            findings.append(
                "GFX24 contract qualification test preset must select exactly "
                "suite.gfx24-qualification"
            )

    if (
        GFX24_CANDIDATE_CONFIGURE_PRESET not in configure_names
        or GFX24_CANDIDATE_BUILD_PRESET not in build_names
        or GFX24_CANDIDATE_TEST_BASE not in test_names
        or GFX24_CANDIDATE_TEST_PRESET not in test_names
    ):
        findings.append(
            "GFX24 native candidate preset triplet is incomplete: "
            f"{GFX24_CANDIDATE_CONFIGURE_PRESET}/{GFX24_CANDIDATE_TEST_PRESET}"
        )
    else:
        gfx24_candidate_configure_preset = next(
            entry
            for entry in presets["configurePresets"]
            if entry["name"] == GFX24_CANDIDATE_CONFIGURE_PRESET
        )
        candidate_cache_variables = gfx24_candidate_configure_preset.get(
            "cacheVariables"
        )
        if (
            not isinstance(candidate_cache_variables, dict)
            or candidate_cache_variables.get("CMAKE_BUILD_TYPE") != "Release"
            or candidate_cache_variables.get("RVX_ENABLE_GFX24_NATIVE_CANDIDATES")
            != "ON"
            or candidate_cache_variables.get("RVX_ENABLE_GFX24_QUALIFICATION")
            != "OFF"
        ):
            findings.append(
                "GFX24 native candidate configure preset must be isolated "
                "Release evidence with only RVX_ENABLE_GFX24_NATIVE_CANDIDATES enabled"
            )
        gfx24_candidate_build_preset = next(
            entry
            for entry in presets["buildPresets"]
            if entry["name"] == GFX24_CANDIDATE_BUILD_PRESET
        )
        if (
            gfx24_candidate_build_preset.get("configurePreset")
            != GFX24_CANDIDATE_CONFIGURE_PRESET
            or gfx24_candidate_build_preset.get("configuration") != "Release"
            or gfx24_candidate_build_preset.get("targets")
            != [GFX24_CANDIDATE_BUILD_TARGET]
        ):
            findings.append(
                "GFX24 native candidate build preset must build only "
                "RVXValidationSuiteGfx24Candidate in its Release tree"
            )
        gfx24_candidate_test_preset = next(
            entry
            for entry in presets["testPresets"]
            if entry["name"] == GFX24_CANDIDATE_TEST_PRESET
        )
        gfx24_candidate_test_base = next(
            entry
            for entry in presets["testPresets"]
            if entry["name"] == GFX24_CANDIDATE_TEST_BASE
        )
        if (
            gfx24_candidate_test_preset.get("inherits")
            != GFX24_CANDIDATE_TEST_BASE
            or gfx24_candidate_test_base.get("configurePreset")
            != GFX24_CANDIDATE_CONFIGURE_PRESET
            or gfx24_candidate_test_base.get("configuration") != "Release"
        ):
            findings.append(
                "GFX24 native candidate test preset must inherit its fixed "
                "Release candidate configure preset"
            )
        expected_filter = {
            "include": {"label": "^suite\\.gfx24-candidate$"}
        }
        if gfx24_candidate_test_preset.get("filter") != expected_filter:
            findings.append(
                "GFX24 native candidate test preset must select exactly "
                "suite.gfx24-candidate"
            )
    if "win_x64_release" in configure_names:
        generic_release_preset = next(
            entry
            for entry in presets["configurePresets"]
            if entry["name"] == "win_x64_release"
        )
        generic_release_cache = generic_release_preset.get("cacheVariables")
        if (
            isinstance(generic_release_cache, dict)
            and generic_release_cache.get("RVX_ENABLE_GFX24_NATIVE_CANDIDATES")
            == "ON"
        ):
            findings.append(
                "GFX24 native candidates must remain out of the generic "
                "Windows Release preset"
            )

    expected_raster_filter = {
        "include": {"label": "^suite\\.gfx24-raster-correctness$"}
    }
    for platform, contract in GFX24_RASTER_CONTRACTS.items():
        configure = contract["configure"]
        test = contract["test"]
        if (
            configure not in configure_names
            or configure not in build_names
            or test not in test_names
        ):
            findings.append(
                "GFX24 raster correctness preset triplet is incomplete: "
                f"{platform}/{configure}/{test}"
            )
            continue
        build_preset = next(
            entry for entry in presets["buildPresets"] if entry["name"] == configure
        )
        if (
            build_preset.get("configurePreset") != configure
            or build_preset.get("configuration") not in (None, "Release")
            or build_preset.get("targets") != [GFX24_RASTER_BUILD_TARGET]
        ):
            findings.append(
                "GFX24 raster correctness build preset must build only "
                f"{GFX24_RASTER_BUILD_TARGET}: {platform}"
            )
        test_preset = next(
            entry for entry in presets["testPresets"] if entry["name"] == test
        )
        if test_preset.get("filter") != expected_raster_filter:
            findings.append(
                "GFX24 raster correctness test preset must select exactly "
                f"{GFX24_RASTER_LABEL}: {platform}"
            )

    for job_name, contract in GFX24_HARDWARE_RT_CONTRACTS.items():
        configure = contract["configure"]
        test = contract["test"]
        target = contract["target"]
        label = contract["label"]
        if (
            configure not in configure_names
            or configure not in build_names
            or test not in test_names
        ):
            findings.append(
                "GFX24 hardware ray-tracing preset triplet is incomplete: "
                f"{job_name}/{configure}/{test}"
            )
            continue
        build_preset = next(
            entry for entry in presets["buildPresets"] if entry["name"] == configure
        )
        if (
            build_preset.get("configurePreset") != configure
            or build_preset.get("configuration") not in (None, "Release")
            or build_preset.get("targets") != [target]
        ):
            findings.append(
                "GFX24 hardware ray-tracing build preset must build only "
                f"{target}: {job_name}"
            )
        test_preset = next(
            entry for entry in presets["testPresets"] if entry["name"] == test
        )
        expected_filter = {
            "include": {"label": "^" + str(label).replace(".", "\\.") + "$"}
        }
        if test_preset.get("filter") != expected_filter:
            findings.append(
                "GFX24 hardware ray-tracing test preset must select exactly "
                f"{label}: {job_name}"
            )

    for configure, test in PR_SMOKE_PRESETS.items():
        if configure not in configure_names or configure not in build_names or test not in test_names:
            findings.append(f"PR smoke preset triplet is incomplete: {configure}/{test}")
    for configure, test in SHIPPING_PRESETS.items():
        if configure not in configure_names or configure not in build_names or test not in test_names:
            findings.append(f"Shipping preset triplet is incomplete: {configure}/{test}")
    for preset in COMPATIBILITY_PRESETS | WEEKLY_PRESETS:
        if preset not in configure_names or preset not in build_names or preset not in test_names:
            findings.append(f"scheduled validation preset triplet is incomplete: {preset}")

    ci_jobs = workflow_jobs.get("ci.yml", {})
    architecture = ci_jobs.get("architecture", "")
    for token in (
        "RVXValidationArchitecture",
        "linux_x64_debug_architecture",
        "Scripts/run_required_suite.ps1",
        "-RequiredLabel lane.architecture",
        'unittest discover -s Scripts -p "test_*.py"',
        "Scripts/plan_validation.py",
        "--mode observation",
        "ValidationSelectionPlan.json",
        "git diff --check",
    ):
        if token not in architecture:
            findings.append(f"ci.yml architecture job is missing {token}")

    if re.search(r"Scripts/plan_validation\.py(?:(?!\n\s*- name:).)*--mode\s+selective", architecture, re.DOTALL):
        findings.append("ci.yml must keep dependency-aware selection in observation mode")

    debug_contracts = {
        "windows-debug": (
            "win_x64_debug_unit,win_x64_debug_integration,win_x64_debug_native",
            "win_x64_pr_debug_smoke",
        ),
        "linux-debug": (
            "linux_x64_debug_unit,linux_x64_debug_integration,linux_x64_debug_native",
            "linux_x64_pr_debug_smoke",
        ),
        "macos-arm64": (
            "mac_arm64_debug_unit,mac_arm64_debug_integration,mac_arm64_debug_native",
            "mac_arm64_pr_debug_smoke",
        ),
    }
    for job_name, (lanes, smoke) in debug_contracts.items():
        block = ci_jobs.get(job_name, "")
        lane_match = re.search(r"-LaneTestPresets\s+[\"']([^\"']+)[\"']", block)
        actual_lanes = lane_match.group(1) if lane_match else ""
        if actual_lanes != lanes:
            findings.append(f"ci.yml {job_name} must run the exact Unit/Integration/Native lane set")
        if smoke not in block:
            findings.append(f"ci.yml {job_name} is missing fixed PR smoke preset {smoke}")
        for token in (
            "Scripts/run_required_suite.ps1",
            "RVXValidationSuitePrSmoke",
            "-RequiredLabel suite.pr-smoke",
            "-ExpectedTestCount 3",
        ):
            if token not in block:
                findings.append(f"ci.yml {job_name} is missing fail-closed PR smoke token {token}")

    tsan = ci_jobs.get("linux-tsan", "")
    if "RenderConcurrencyTSAN" not in tsan or "linux_x64_tsan" not in tsan:
        findings.append("ci.yml Linux TSAN must build and run the focused concurrency target")
    for token in ("Scripts/run_required_suite.ps1", "-RequiredLabel lane.integration"):
        if token not in tsan:
            findings.append(f"ci.yml Linux TSAN is missing fail-closed token {token}")

    shipping = ci_jobs.get("shipping", "")
    for token in (*SHIPPING_PRESETS.keys(), *SHIPPING_PRESETS.values()):
        if token not in shipping:
            findings.append(f"ci.yml Shipping matrix is missing {token}")
    for platform in ("Windows", "Linux", "macOS"):
        if f"platform: {platform}" not in shipping:
            findings.append(f"ci.yml Shipping matrix is missing {platform}")
    for token in (
        "Scripts/run_required_suite.ps1",
        "RVXValidationSuiteRequiredShipping",
        "-RequiredLabel suite.required-shipping",
        "-ExpectedTestCount 5",
        "build/*shipping_release/Tests/Shipping/ShippingFixtures",
        "if-no-files-found: error",
    ):
        if token not in shipping:
            findings.append(f"ci.yml Shipping is missing fail-closed token {token}")

    raster = ci_jobs.get("raster-correctness", "")
    for token in (
        "strategy:",
        "matrix:",
        "runs-on: ${{ matrix.runner }}",
        'RVX_NATIVE_ALLOW_SOFTWARE_ADAPTER: "1"',
        "Scripts/run_required_suite.ps1",
        GFX24_RASTER_BUILD_TARGET,
        f"-RequiredLabel {GFX24_RASTER_LABEL}",
        "-ExpectedTestCount 4",
        "Tests/Gfx24RasterCorrectness",
        "if-no-files-found: error",
    ):
        if token not in raster:
            findings.append(f"ci.yml GFX24 raster correctness is missing {token}")
    for platform, contract in GFX24_RASTER_CONTRACTS.items():
        for token in (
            f"platform: {platform}",
            f"configure_preset: {contract['configure']}",
            f"build_preset: {contract['configure']}",
            f"test_preset: {contract['test']}",
            f"build_dir: {contract['build_dir']}",
            f"backend: {contract['backend']}",
            *contract["evidence"],
        ):
            if token not in raster:
                findings.append(
                    "ci.yml GFX24 raster correctness is missing required "
                    f"{platform} token {token}"
                )

    for job_name, contract in GFX24_HARDWARE_RT_CONTRACTS.items():
        hardware = ci_jobs.get(job_name, "")
        for token in (
            f"runs-on: {contract['runner']}",
            "Scripts/run_required_suite.ps1",
            str(contract["configure"]),
            str(contract["test"]),
            str(contract["build_dir"]),
            str(contract["target"]),
            f"-RequiredLabel {contract['label']}",
            f"-ExpectedTestCount {contract['count']}",
            "BuildTruth",
            ".agent-harness/receipts/RequiredSuite.*.json",
            "if-no-files-found: error",
            *contract["evidence"],
        ):
            if token not in hardware:
                findings.append(
                    f"ci.yml {job_name} is missing required hardware token {token}"
                )
        for forbidden in (
            "RVX_NATIVE_ALLOW_SOFTWARE_ADAPTER",
            "-AllowSoftwareAdapter",
            "LIBGL_ALWAYS_SOFTWARE",
            "VK_DRIVER_FILES",
            "VK_ICD_FILENAMES",
            "--allow-fallback",
            "-SkipConfigure",
        ):
            if forbidden in hardware:
                findings.append(
                    f"ci.yml {job_name} must not permit software, fallback, or skipped hardware validation: {forbidden}"
                )

    windows_hardware = ci_jobs.get("windows-gfx24-hardware", "")
    for token in (
        "Scripts/run_render_quality_performance_calibration.ps1",
        "win_x64_gfx24_performance_calibration_release",
        "win_x64_gfx24_performance_calibration_release_run",
        "build/gfx24_perf_calibration",
        "PerformanceCalibration",
    ):
        if token not in windows_hardware:
            findings.append(
                f"ci.yml Windows GFX24 hardware job is missing performance token {token}"
            )

    linux_hardware = ci_jobs.get("linux-gfx24-vulkan-rt", "")
    for token in (
        "Verify Vulkan hardware adapter",
        "vulkaninfo --summary",
        "lavapipe|llvmpipe|swiftshader",
    ):
        if token not in linux_hardware:
            findings.append(
                f"ci.yml Linux GFX24 hardware job is missing adapter gate {token}"
            )

    verdict = ci_jobs.get("pr-verdict", "")
    if "if: always()" not in verdict:
        findings.append("ci.yml PR Verdict must run with if: always()")
    needs_match = re.search(r"^\s+needs:\s*\[([^]]+)\]", verdict, re.MULTILINE)
    actual_needs = {
        item.strip() for item in needs_match.group(1).split(",")
    } if needs_match else set()
    required_needs = WORKFLOWS["ci.yml"] - {"pr-verdict"}
    for required in required_needs:
        if required not in actual_needs:
            findings.append(f"ci.yml PR Verdict does not require {required}")
    if actual_needs - required_needs:
        findings.append(f"ci.yml PR Verdict has unexpected needs {sorted(actual_needs - required_needs)}")
    if 'test "$result" = success || exit 1' not in verdict:
        findings.append("ci.yml PR Verdict must fail unless every required job succeeds")

    nightly = workflow_text.get("nightly.yml", "")
    for token in (
        "schedule:",
        "win_x64_qualification_debug_qualification",
        "win_x64_nightly_compatibility_debug",
        "linux_x64_nightly_compatibility_debug",
        "linux_x64_tsan_integration",
        "mac_arm64_debug_unit",
        "RVX_EXTERNAL_ASSET_URL",
        "RVX_EXTERNAL_ASSET_SHA256",
        "RVX_ENABLE_EXTERNAL_ASSET_TESTS=ON",
        "Scripts/run_required_suite.ps1",
        "RVXValidationSuiteCompatibilitySmoke",
        "-RequiredLabel lane.qualification",
    ):
        if token not in nightly:
            findings.append(f"nightly.yml is missing required coverage token {token}")

    gfx15_step = re.search(
        r"^\s*- name: Run fail-closed GFX15 translucency matrix\s*$"
        r"(?P<body>.*?)(?=^\s*- (?:name:|if:)|\Z)",
        nightly,
        re.MULTILINE | re.DOTALL,
    )
    if gfx15_step is None:
        findings.append("nightly.yml is missing the fail-closed GFX15 translucency step")
    else:
        gfx15_body = gfx15_step.group("body")
        for token in ("Scripts/run_required_suite.ps1",):
            if token not in gfx15_body:
                findings.append(f"nightly.yml GFX15 step is missing {token}")
        for argument, expected in (
            ("ConfigurePreset", GFX15_CONFIGURE_PRESET),
            ("BuildPreset", GFX15_CONFIGURE_PRESET),
            ("TestPreset", GFX15_TEST_PRESET),
            ("BuildTarget", GFX15_BUILD_TARGET),
            ("RequiredLabel", GFX15_LABEL),
            ("ExpectedTestCount", "12"),
        ):
            argument_match = re.search(rf"-{argument}\s+(\S+)", gfx15_body)
            if argument_match is None or argument_match.group(1) != expected:
                findings.append(
                    f"nightly.yml GFX15 step must set -{argument} {expected}"
                )
        evidence_match = re.search(r'-RequiredEvidence\s+"([^"]*)"', gfx15_body)
        if evidence_match is None:
            findings.append("nightly.yml GFX15 step is missing RequiredEvidence")
        else:
            actual_evidence = tuple(
                item for item in evidence_match.group(1).split(";") if item
            )
            if (
                len(actual_evidence) != 12
                or len(set(actual_evidence)) != 12
                or set(actual_evidence) != GFX15_REQUIRED_EVIDENCE
            ):
                findings.append(
                    "nightly.yml GFX15 RequiredEvidence must contain the exact "
                    "four image/report/startup evidence triplets"
                )

    gfx18_step = re.search(
        r"^\s*- name: Run fail-closed GFX18 water matrix\s*$"
        r"(?P<body>.*?)(?=^\s*- (?:name:|if:)|\Z)",
        nightly,
        re.MULTILINE | re.DOTALL,
    )
    if gfx18_step is None:
        findings.append("nightly.yml is missing the fail-closed GFX18 water step")
    else:
        gfx18_body = gfx18_step.group("body")
        if "Scripts/run_required_suite.ps1" not in gfx18_body:
            findings.append("nightly.yml GFX18 step is missing Scripts/run_required_suite.ps1")
        for argument, expected in (
            ("ConfigurePreset", GFX18_CONFIGURE_PRESET),
            ("BuildPreset", GFX18_CONFIGURE_PRESET),
            ("TestPreset", GFX18_TEST_PRESET),
            ("BuildTarget", GFX18_BUILD_TARGET),
            ("RequiredLabel", GFX18_LABEL),
            ("ExpectedTestCount", "12"),
        ):
            argument_match = re.search(rf"-{argument}\s+(\S+)", gfx18_body)
            if argument_match is None or argument_match.group(1) != expected:
                findings.append(
                    f"nightly.yml GFX18 step must set -{argument} {expected}"
                )
        evidence_match = re.search(r'-RequiredEvidence\s+"([^"]*)"', gfx18_body)
        if evidence_match is None:
            findings.append("nightly.yml GFX18 step is missing RequiredEvidence")
        else:
            actual_evidence = tuple(
                item for item in evidence_match.group(1).split(";") if item
            )
            if (
                len(actual_evidence) != 12
                or len(set(actual_evidence)) != 12
                or set(actual_evidence) != GFX18_REQUIRED_EVIDENCE
            ):
                findings.append(
                    "nightly.yml GFX18 RequiredEvidence must contain the exact "
                    "four image/report/startup evidence triplets"
                )

    weekly = workflow_text.get("weekly.yml", "")
    for token in (
        "schedule:",
        "Windows-DX12",
        "Linux-Vulkan",
        "macOS-Metal",
        *sorted(WEEKLY_PRESETS),
        "Scripts/run_required_suite.ps1",
        "RVXValidationSuiteWeeklySoak",
        "-RequiredLabel suite.weekly-soak",
        "-ExpectedTestCount 2",
    ):
        if token not in weekly:
            findings.append(f"weekly.yml is missing required soak token {token}")
    qualification = _read(root / "Tests" / "CMake" / "QualificationTests.cmake")
    test_support = _read(root / "Tests" / "CMake" / "TestSupport.cmake")
    unit = _read(root / "Tests" / "CMake" / "UnitTests.cmake")
    pass_timing = _read(
        root / "Tests" / "RenderGraphPassTimingValidation" / "CMakeLists.txt"
    )
    for token in (
        "if(RVX_ENABLE_GFX24_QUALIFICATION)",
        "RenderQualityMetricsValidation",
        "RenderQualityMetricsValidationSelfTest",
        "Scripts/check_render_quality_series.py",
        "Scripts/render_quality_golden.py",
        "Scripts/test_render_quality_qualification.py",
        "SUITE gfx24-qualification",
    ):
        if token not in unit:
            findings.append(
                f"GFX24 contract qualification registration is missing {token}"
            )
    for token in (
        "option(RVX_ENABLE_GFX24_NATIVE_CANDIDATES",
        "Register opt-in DX12 cooked RenderQualityLab visual/performance evidence producers",
    ):
        if token not in test_support:
            findings.append(
                f"GFX24 native candidate opt-in registration is missing {token}"
            )

    gfx24_candidate_function_match = re.search(
        r"function\(rvx_add_renderverse_gfx24_native_candidates\b(?P<body>.*?)\nendfunction\(\)",
        qualification,
        re.DOTALL,
    )
    gfx24_candidate_function = (
        gfx24_candidate_function_match.group("body")
        if gfx24_candidate_function_match
        else ""
    )
    if not gfx24_candidate_function:
        findings.append("GFX24 native candidate producer implementation function is missing")
    else:
        for token in (
            "RenderVerseSamplesCooked",
            "RVXCook",
            "--cooked-shader-root",
            "--pipeline deferred",
            "--quality high",
            "--fixed-delta 0.0166666666666667",
            "--width 1280",
            "--height 720",
            "RenderVerseSamplesCookedDX12Gfx24DeferredHighSeriesCandidate",
            "--qualification-series",
            "series.json",
            "--width 2560",
            "--height 1440",
            "--qualification-performance",
            "performance.json",
            "--qualification-performance-baseline gfx24-dx12-candidate-unapproved",
            "--no-validation",
            "--capture-tick 300",
            "SUITE gfx24-candidate",
        ):
            if token not in gfx24_candidate_function:
                findings.append(
                    f"GFX24 native candidate producer is missing required token {token}"
                )
        # The same-process Series producer captures 120/360/361, while the
        # second registration remains the fixed performance producer.
        if gfx24_candidate_function.count("SUITE gfx24-candidate") != 2:
            findings.append(
                "GFX24 native candidate suite must register one fixed visual "
                "Series producer and one performance producer"
            )
        if len(re.findall(r"\bRESOURCE_LOCK rvx_gpu\b", gfx24_candidate_function)) != 2:
            findings.append(
                "GFX24 native candidate GPU lock must be held only by its "
                "visual Series producer and performance producer"
            )
        if (
            gfx24_candidate_function.count("--validation") != 1
            or gfx24_candidate_function.count("--no-validation") != 1
        ):
            findings.append(
                "GFX24 visual producer must keep validation enabled while its "
                "performance producer must disable the API debug layer"
            )
        for forbidden in (
            "$<TARGET_FILE:RenderVerseSamples>",
            "--sample",
            "--runtime-only-composition",
            "--asset-root",
            "--catalog",
            "ImageContentValidation",
            "VisualGoldenValidation",
            "SampleReportContentValidation",
            "RenderQualityMetricsValidation",
        ):
            if forbidden in gfx24_candidate_function:
                findings.append(
                    "GFX24 native candidate producer violates the cooked-only "
                    f"candidate boundary with forbidden token {forbidden}"
                )

    gfx24_calibration_function_match = re.search(
        r"function\(rvx_add_renderverse_gfx24_performance_calibration\b(?P<body>.*?)\nendfunction\(\)",
        qualification,
        re.DOTALL,
    )
    gfx24_calibration_function = (
        gfx24_calibration_function_match.group("body")
        if gfx24_calibration_function_match
        else ""
    )
    if not gfx24_calibration_function:
        findings.append("GFX24 performance calibration implementation function is missing")
    elif "--capture-tick 300" not in gfx24_calibration_function:
        findings.append(
            "GFX24 performance calibration must capture after the 300-frame warmup"
        )

    samples = sample_manifest.get("samples")
    if not isinstance(samples, list):
        findings.append("RenderVerseSamples manifest has no samples array")
    else:
        render_quality_lab = next(
            (entry for entry in samples
             if isinstance(entry, dict) and entry.get("id") == "render-quality-lab"),
            None,
        )
        if not isinstance(render_quality_lab, dict) or (
            render_quality_lab.get("captureTicks") != [300, 360, 361]
        ):
            findings.append(
                "RenderQualityLab must keep the GFX24 raster capture ticks 300/360/361"
            )
    for token in (
        "if(RVX_ENABLE_GFX24_NATIVE_CANDIDATES)",
        "RVX_ENABLE_GFX24_NATIVE_CANDIDATES requires Windows DX12, "
        "RenderVerseSamplesCooked, and RVXCook",
        "rvx_add_renderverse_gfx24_native_candidates()",
    ):
        if token not in qualification:
            findings.append(
                f"GFX24 native candidate registration is missing {token}"
            )
    for token in (
        "NAME RenderGraphPassTimingValidation",
        "RVX::RenderFoundation",
        "SUITE gfx24-qualification",
    ):
        if token not in pass_timing:
            findings.append(
                f"GFX24 pass timestamp contract registration is missing {token}"
            )
    gfx18_function_match = re.search(
        r"function\(rvx_add_renderverse_gfx18_water_native\b(?P<body>.*?)\nendfunction\(\)",
        qualification,
        re.DOTALL,
    )
    gfx18_function = gfx18_function_match.group("body") if gfx18_function_match else ""
    if not gfx18_function:
        findings.append("GFX18 water qualification implementation function is missing")
    else:
        for token in (
            "--runtime-only-composition",
            "--cooked-shader-root",
            "--require-cooked-shader-provider",
            "--require-water-surface-execution",
            "SUITE gfx18-water",
        ):
            if token not in gfx18_function:
                findings.append(
                    f"GFX18 water qualification is missing required token {token}"
                )
        if gfx18_function.count("SUITE gfx18-water") != 3:
            findings.append(
                "GFX18 water qualification must register exactly Capture/Image/Receipt"
            )
    weekly_function_match = re.search(
        r"function\(rvx_add_renderverse_weekly_soak\b(?P<body>.*?)\nendfunction\(\)",
        qualification,
        re.DOTALL,
    )
    weekly_function = weekly_function_match.group("body") if weekly_function_match else ""
    if not weekly_function:
        findings.append("weekly soak implementation function is missing")
    for token in (
        "--lifetime-warmup-frames 120",
        "--lifetime-observation-frames 9880",
        "--lifetime-min-duration-ms 300000",
        "suite.soak suite.weekly-soak",
    ):
        if token not in weekly_function:
            findings.append(f"weekly soak implementation is missing {token}")
    for token in ("RVX_ENABLE_WEEKLY_SOAK", "RVX_ENABLE_COMPATIBILITY_SMOKE"):
        if token not in qualification:
            findings.append(f"scheduled qualification mode is missing {token}")

    return findings


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    arguments = parser.parse_args()
    root = arguments.root.resolve()
    try:
        findings = validate(root)
    except ValueError as error:
        print(f"CI workflow input error: {error}", file=sys.stderr)
        return 2
    if findings:
        print("CI workflow policy failed:", file=sys.stderr)
        for finding in findings:
            print(f"  - {finding}", file=sys.stderr)
        return 1
    print("CI workflow policy passed: required matrix is complete and dependency planning is observation-only")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
