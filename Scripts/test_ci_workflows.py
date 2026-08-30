#!/usr/bin/env python3
"""Self-tests for the CI workflow policy gate."""

from __future__ import annotations

import json
import shutil
import tempfile
import unittest
from pathlib import Path

from check_ci_workflows import validate


class CIWorkflowPolicyTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.repository = Path(__file__).resolve().parents[1]

    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        shutil.copytree(
            self.repository / ".github" / "workflows",
            self.root / ".github" / "workflows",
        )
        shutil.copy2(self.repository / "CMakePresets.json", self.root)
        qualification = self.root / "Tests" / "CMake"
        qualification.mkdir(parents=True)
        shutil.copy2(
            self.repository / "Tests" / "CMake" / "QualificationTests.cmake",
            qualification,
        )
        shutil.copy2(
            self.repository / "Tests" / "CMake" / "UnitTests.cmake",
            qualification,
        )
        shutil.copy2(
            self.repository / "Tests" / "CMake" / "TestSupport.cmake",
            qualification,
        )
        timing = self.root / "Tests" / "RenderGraphPassTimingValidation"
        timing.mkdir(parents=True)
        shutil.copy2(
            self.repository / "Tests" / "RenderGraphPassTimingValidation" / "CMakeLists.txt",
            timing,
        )

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def replace(self, relative: str, old: str, new: str) -> None:
        path = self.root / relative
        text = path.read_text(encoding="utf-8")
        self.assertIn(old, text)
        path.write_text(text.replace(old, new, 1), encoding="utf-8")

    def replace_gfx24_candidate_function(self, old: str, new: str) -> None:
        path = self.root / "Tests" / "CMake" / "QualificationTests.cmake"
        text = path.read_text(encoding="utf-8")
        marker = "function(rvx_add_renderverse_gfx24_native_candidates)"
        prefix, remainder = text.split(marker, 1)
        body, suffix = remainder.split("\nendfunction()", 1)
        self.assertIn(old, body)
        path.write_text(
            prefix + marker + body.replace(old, new) + "\nendfunction()" + suffix,
            encoding="utf-8",
        )

    def test_repository_contract_passes(self) -> None:
        self.assertEqual(validate(self.root), [])

    def test_missing_required_job_fails_closed(self) -> None:
        self.replace(".github/workflows/ci.yml", "  linux-tsan:\n", "  linux-tsan-removed:\n")
        findings = validate(self.root)
        self.assertTrue(any("jobs must be exactly" in item for item in findings))

    def test_verdict_must_require_shipping(self) -> None:
        self.replace(
            ".github/workflows/ci.yml",
            "needs: [architecture, windows-debug, linux-debug, linux-tsan, macos-arm64, shipping]",
            "needs: [architecture, windows-debug, linux-debug, linux-tsan, macos-arm64]",
        )
        findings = validate(self.root)
        self.assertTrue(any("does not require shipping" in item for item in findings))

    def test_pr_jobs_cannot_duplicate_architecture(self) -> None:
        self.replace(
            ".github/workflows/ci.yml",
            "win_x64_debug_unit,win_x64_debug_integration,win_x64_debug_native",
            "win_x64_debug_architecture,win_x64_debug_unit,win_x64_debug_integration,win_x64_debug_native",
        )
        findings = validate(self.root)
        self.assertTrue(any("exact Unit/Integration/Native" in item for item in findings))

    def test_dependency_planner_must_remain_observation_only(self) -> None:
        self.replace(
            ".github/workflows/ci.yml",
            "--mode observation",
            "--mode selective",
        )
        findings = validate(self.root)
        self.assertTrue(any("observation" in item for item in findings))

    def test_dependency_planner_evidence_must_be_uploaded(self) -> None:
        self.replace(
            ".github/workflows/ci.yml",
            "ValidationSelectionPlan.json",
            "SelectionPlan.removed.json",
        )
        findings = validate(self.root)
        self.assertTrue(any("ValidationSelectionPlan.json" in item for item in findings))

    def test_missing_preset_fails_closed(self) -> None:
        path = self.root / "CMakePresets.json"
        data = json.loads(path.read_text(encoding="utf-8"))
        data["testPresets"] = [
            item for item in data["testPresets"] if item["name"] != "mac_arm64_pr_debug_smoke"
        ]
        path.write_text(json.dumps(data), encoding="utf-8")
        findings = validate(self.root)
        self.assertTrue(any("PR smoke preset triplet is incomplete" in item for item in findings))

    def test_weekly_duration_is_a_real_runtime_gate(self) -> None:
        path = self.root / "Tests/CMake/QualificationTests.cmake"
        text = path.read_text(encoding="utf-8")
        marker = "function(rvx_add_renderverse_weekly_soak"
        prefix, weekly = text.split(marker, 1)
        self.assertIn("--lifetime-min-duration-ms 300000", weekly)
        path.write_text(
            prefix + marker + weekly.replace(
                "--lifetime-min-duration-ms 300000",
                "--lifetime-min-duration-ms 1",
                1,
            ),
            encoding="utf-8",
        )
        findings = validate(self.root)
        self.assertTrue(any("--lifetime-min-duration-ms 300000" in item for item in findings))

    def test_gfx15_step_requires_exact_target_label_and_count(self) -> None:
        self.replace(
            ".github/workflows/nightly.yml",
            "-BuildTarget RVXValidationSuiteGfx15Translucency",
            "-BuildTarget RVXValidationSuiteGfx15TranslucencyBroken",
        )
        findings = validate(self.root)
        self.assertTrue(any("RVXValidationSuiteGfx15Translucency" in item
                            for item in findings))

    def test_gfx15_step_rejects_missing_or_substituted_evidence(self) -> None:
        self.replace(
            ".github/workflows/nightly.yml",
            "RenderVerseSamplesDX12GFX15ForwardPlusTranslucency_640x360.ppm",
            "RenderVerseSamplesDX12GFX15ForwardPlusTranslucency_missing.ppm",
        )
        findings = validate(self.root)
        self.assertTrue(any("GFX15 RequiredEvidence" in item for item in findings))

    def test_gfx15_preset_must_select_the_exact_suite_label(self) -> None:
        path = self.root / "CMakePresets.json"
        data = json.loads(path.read_text(encoding="utf-8"))
        preset = next(
            item for item in data["testPresets"]
            if item["name"] == "win_x64_qualification_debug_gfx15_translucency"
        )
        preset["filter"] = {"include": {"label": "^suite\\.wrong$"}}
        path.write_text(json.dumps(data), encoding="utf-8")
        findings = validate(self.root)
        self.assertTrue(any("GFX15 translucency test preset" in item
                            for item in findings))

    def test_gfx18_step_requires_exact_target_label_and_count(self) -> None:
        self.replace(
            ".github/workflows/nightly.yml",
            "-BuildTarget RVXValidationSuiteGfx18Water",
            "-BuildTarget RVXValidationSuiteGfx18WaterBroken",
        )
        findings = validate(self.root)
        self.assertTrue(any("RVXValidationSuiteGfx18Water" in item
                            for item in findings))

    def test_gfx18_step_rejects_missing_or_substituted_evidence(self) -> None:
        self.replace(
            ".github/workflows/nightly.yml",
            "RenderVerseSamplesDX12GFX18ForwardPlusWaterSurface_640x360.ppm",
            "RenderVerseSamplesDX12GFX18ForwardPlusWaterSurface_missing.ppm",
        )
        findings = validate(self.root)
        self.assertTrue(any("GFX18 RequiredEvidence" in item for item in findings))

    def test_gfx18_preset_must_select_the_exact_suite_label(self) -> None:
        path = self.root / "CMakePresets.json"
        data = json.loads(path.read_text(encoding="utf-8"))
        preset = next(
            item for item in data["testPresets"]
            if item["name"] == "win_x64_qualification_debug_gfx18_water"
        )
        preset["filter"] = {"include": {"label": "^suite\\.wrong$"}}
        path.write_text(json.dumps(data), encoding="utf-8")
        findings = validate(self.root)
        self.assertTrue(any("GFX18 water test preset" in item
                            for item in findings))

    def test_gfx18_qualification_requires_runtime_only_cooked_provider(self) -> None:
        self.replace(
            "Tests/CMake/QualificationTests.cmake",
            "--require-cooked-shader-provider",
            "--require-development-shader-provider",
        )
        findings = validate(self.root)
        self.assertTrue(any("--require-cooked-shader-provider" in item
                            for item in findings))

    def test_gfx24_preset_must_enable_the_contract_suite(self) -> None:
        self.replace(
            "CMakePresets.json",
            '"RVX_ENABLE_GFX24_QUALIFICATION": "ON"',
            '"RVX_ENABLE_GFX24_QUALIFICATION": "OFF"',
        )
        findings = validate(self.root)
        self.assertTrue(any("GFX24 contract qualification must be enabled" in item
                            for item in findings))

    def test_gfx24_preset_must_keep_the_exact_contract_label(self) -> None:
        self.replace(
            "CMakePresets.json",
            "^suite\\\\.gfx24-qualification$",
            "^suite\\\\.wrong$",
        )
        findings = validate(self.root)
        self.assertTrue(any("GFX24 contract qualification test preset" in item
                            for item in findings))

    def test_gfx24_candidate_preset_must_enable_only_the_native_producers(self) -> None:
        self.replace(
            "CMakePresets.json",
            '"RVX_ENABLE_GFX24_NATIVE_CANDIDATES": "ON"',
            '"RVX_ENABLE_GFX24_NATIVE_CANDIDATES": "OFF"',
        )
        findings = validate(self.root)
        self.assertTrue(any("GFX24 native candidate configure preset" in item
                            for item in findings))

    def test_gfx24_candidate_cannot_pollute_generic_release(self) -> None:
        path = self.root / "CMakePresets.json"
        data = json.loads(path.read_text(encoding="utf-8"))
        preset = next(
            item for item in data["configurePresets"]
            if item["name"] == "win_x64_release"
        )
        preset["cacheVariables"]["RVX_ENABLE_GFX24_NATIVE_CANDIDATES"] = "ON"
        path.write_text(json.dumps(data), encoding="utf-8")
        findings = validate(self.root)
        self.assertTrue(any("must remain out of the generic" in item
                            for item in findings))

    def test_gfx24_candidate_preset_must_keep_the_exact_candidate_label(self) -> None:
        path = self.root / "CMakePresets.json"
        data = json.loads(path.read_text(encoding="utf-8"))
        preset = next(
            item for item in data["testPresets"]
            if item["name"] == "win_x64_gfx24_candidate_release_gfx24_candidate"
        )
        preset["filter"] = {"include": {"label": "^suite\\.wrong$"}}
        path.write_text(json.dumps(data), encoding="utf-8")
        findings = validate(self.root)
        self.assertTrue(any("GFX24 native candidate test preset" in item
                            for item in findings))

    def test_gfx24_candidate_test_preset_must_remain_in_its_release_tree(self) -> None:
        path = self.root / "CMakePresets.json"
        data = json.loads(path.read_text(encoding="utf-8"))
        preset = next(
            item for item in data["testPresets"]
            if item["name"] == "win_x64_gfx24_candidate_release_lane_base"
        )
        preset["configuration"] = "Debug"
        path.write_text(json.dumps(data), encoding="utf-8")
        findings = validate(self.root)
        self.assertTrue(any("fixed Release candidate configure preset" in item
                            for item in findings))

    def test_gfx24_candidate_requires_the_cooked_shader_boundary(self) -> None:
        self.replace_gfx24_candidate_function(
            "--cooked-shader-root",
            "--source-shader-root",
        )
        findings = validate(self.root)
        self.assertTrue(any("--cooked-shader-root" in item for item in findings))

    def test_gfx24_candidate_rejects_product_boundary_overrides(self) -> None:
        self.replace_gfx24_candidate_function(
            "--backend dx12",
            "--sample render-quality-lab\n                --backend dx12",
        )
        findings = validate(self.root)
        self.assertTrue(any("forbidden token --sample" in item for item in findings))

    def test_gfx24_candidate_locks_only_its_producers(self) -> None:
        self.replace_gfx24_candidate_function(
            "RESOURCE_LOCK rvx_gpu",
            "RESOURCE_LOCK rvx_gpu_missing",
        )
        findings = validate(self.root)
        self.assertTrue(any("GPU lock must be held only" in item for item in findings))

    def test_gfx24_contract_registration_cannot_drop_metrics_self_test(self) -> None:
        self.replace(
            "Tests/CMake/UnitTests.cmake",
            "RenderQualityMetricsValidationSelfTest",
            "MissingRenderQualityMetricsSelfTest",
        )
        findings = validate(self.root)
        self.assertTrue(any("RenderQualityMetricsValidationSelfTest" in item
                            for item in findings))

    def test_gfx24_contract_registration_cannot_drop_pass_timing_test(self) -> None:
        self.replace(
            "Tests/RenderGraphPassTimingValidation/CMakeLists.txt",
            "NAME RenderGraphPassTimingValidation",
            "NAME MissingRenderGraphPassTimingValidation",
        )
        findings = validate(self.root)
        self.assertTrue(any("RenderGraphPassTimingValidation" in item
                            for item in findings))


if __name__ == "__main__":
    unittest.main()
