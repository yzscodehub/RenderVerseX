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
