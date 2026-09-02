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
        shutil.copy2(self.repository / ".gitattributes", self.root)
        shutil.copy2(self.repository / "CMakePresets.json", self.root)
        sample_manifest = self.root / "Samples" / "RenderVerseSamples" / "Manifests"
        sample_manifest.mkdir(parents=True)
        shutil.copy2(
            self.repository / "Samples" / "RenderVerseSamples" / "Manifests"
            / "renderverse-samples.json",
            sample_manifest,
        )
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

    def replace_gfx24_calibration_function(self, old: str, new: str) -> None:
        path = self.root / "Tests" / "CMake" / "QualificationTests.cmake"
        text = path.read_text(encoding="utf-8")
        marker = "function(rvx_add_renderverse_gfx24_performance_calibration)"
        prefix, remainder = text.split(marker, 1)
        body, suffix = remainder.split("\nendfunction()", 1)
        self.assertIn(old, body)
        path.write_text(
            prefix + marker + body.replace(old, new) + "\nendfunction()" + suffix,
            encoding="utf-8",
        )

    def test_repository_contract_passes(self) -> None:
        self.assertEqual(validate(self.root), [])

    def test_render_quality_json_must_disable_eol_conversion(self) -> None:
        self.replace(
            ".gitattributes",
            "Tests/RenderQuality/**/*.json text eol=lf",
            "Tests/RenderQuality/**/*.json text=auto",
        )
        findings = validate(self.root)
        self.assertTrue(any(
            "immutable RenderQuality rule" in item for item in findings
        ))

    def test_missing_required_job_fails_closed(self) -> None:
        self.replace(".github/workflows/ci.yml", "  linux-tsan:\n", "  linux-tsan-removed:\n")
        findings = validate(self.root)
        self.assertTrue(any("jobs must be exactly" in item for item in findings))

    def test_verdict_must_require_shipping(self) -> None:
        self.replace(
            ".github/workflows/ci.yml",
            "needs: [architecture, windows-debug, linux-debug, linux-tsan, macos-arm64, shipping, raster-correctness, windows-gfx24-hardware, linux-gfx24-vulkan-rt]",
            "needs: [architecture, windows-debug, linux-debug, linux-tsan, macos-arm64, raster-correctness, windows-gfx24-hardware, linux-gfx24-vulkan-rt]",
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

    def test_gfx24_performance_candidate_must_disable_the_api_debug_layer(self) -> None:
        self.replace_gfx24_candidate_function(
            "--no-validation",
            "--validation",
        )
        findings = validate(self.root)
        self.assertTrue(any("performance producer must disable" in item
                            for item in findings))

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

    def test_gfx24_raster_job_cannot_be_removed(self) -> None:
        self.replace(
            ".github/workflows/ci.yml",
            "  raster-correctness:\n",
            "  raster-correctness-removed:\n",
        )
        findings = validate(self.root)
        self.assertTrue(any("jobs must be exactly" in item for item in findings))

    def test_gfx24_raster_label_must_remain_exact(self) -> None:
        self.replace(
            "CMakePresets.json",
            "^suite\\\\.gfx24-raster-correctness$",
            "^suite\\\\.gfx24-raster-correctness-removed$",
        )
        findings = validate(self.root)
        self.assertTrue(any("GFX24 raster correctness" in item for item in findings))

    def test_gfx24_raster_preset_removal_fails_closed(self) -> None:
        path = self.root / "CMakePresets.json"
        document = json.loads(path.read_text(encoding="utf-8"))
        document["testPresets"] = [
            entry for entry in document["testPresets"]
            if entry["name"] != "macos_arm64_gfx24_raster_release_run"
        ]
        path.write_text(json.dumps(document), encoding="utf-8")
        findings = validate(self.root)
        self.assertTrue(any("GFX24 raster correctness preset triplet" in item
                            for item in findings))

    def test_gfx24_raster_test_preset_must_remain_internal(self) -> None:
        path = self.root / "CMakePresets.json"
        document = json.loads(path.read_text(encoding="utf-8"))
        preset = next(
            entry for entry in document["testPresets"]
            if entry["name"] == "win_x64_gfx24_raster_release_run"
        )
        preset.pop("vendor")
        path.write_text(json.dumps(document), encoding="utf-8")
        findings = validate(self.root)
        self.assertTrue(any(
            "raster correctness configure/test presets must remain internal"
            in item for item in findings
        ))

    def test_gfx24_raster_required_evidence_cannot_be_substituted(self) -> None:
        self.replace(
            ".github/workflows/ci.yml",
            "Tests/Gfx24RasterCorrectness/Release/Metal/Deferred/series-validation.json",
            "Tests/Gfx24RasterCorrectness/Release/Metal/Deferred/missing.json",
        )
        findings = validate(self.root)
        self.assertTrue(any("required macOS token" in item for item in findings))

    def test_gfx24_verdict_must_require_raster_correctness(self) -> None:
        self.replace(
            ".github/workflows/ci.yml",
            ", raster-correctness, windows-gfx24-hardware",
            ", raster-correctness-removed, windows-gfx24-hardware",
        )
        findings = validate(self.root)
        self.assertTrue(any("does not require raster-correctness" in item
                            for item in findings))

    def test_gfx24_hardware_runner_labels_must_remain_exact(self) -> None:
        self.replace(
            ".github/workflows/ci.yml",
            "runs-on: [self-hosted, Windows, X64, rvx-gpu-dx12-perf, rtx4070ti]",
            "runs-on: [self-hosted, Windows, X64]",
        )
        findings = validate(self.root)
        self.assertTrue(any("windows-gfx24-hardware" in item for item in findings))

    def test_gfx24_hardware_test_preset_must_remain_internal(self) -> None:
        path = self.root / "CMakePresets.json"
        document = json.loads(path.read_text(encoding="utf-8"))
        preset = next(
            entry for entry in document["testPresets"]
            if entry["name"] == "linux_x64_gfx24_hardware_rt_release_run"
        )
        preset.pop("vendor")
        path.write_text(json.dumps(document), encoding="utf-8")
        findings = validate(self.root)
        self.assertTrue(any(
            "hardware ray-tracing configure/test presets must remain internal"
            in item for item in findings
        ))

    def test_gfx24_hardware_jobs_cannot_allow_software_adapter(self) -> None:
        self.replace(
            ".github/workflows/ci.yml",
            "  windows-gfx24-hardware:\n    name: Windows DX12 GFX24 Performance and DXR\n    runs-on: [self-hosted, Windows, X64, rvx-gpu-dx12-perf, rtx4070ti]\n    env:\n      VCPKG_ROOT: C:/vcpkg",
            "  windows-gfx24-hardware:\n    name: Windows DX12 GFX24 Performance and DXR\n    runs-on: [self-hosted, Windows, X64, rvx-gpu-dx12-perf, rtx4070ti]\n    env:\n      VCPKG_ROOT: C:/vcpkg\n      RVX_NATIVE_ALLOW_SOFTWARE_ADAPTER: '1'",
        )
        findings = validate(self.root)
        self.assertTrue(any("must not permit software" in item for item in findings))

    def test_gfx24_candidate_capture_tick_must_follow_warmup(self) -> None:
        self.replace_gfx24_candidate_function(
            "--capture-tick 300",
            "--capture-tick 120",
        )
        findings = validate(self.root)
        self.assertTrue(any("--capture-tick 300" in item for item in findings))

    def test_gfx24_performance_capture_tick_must_follow_warmup(self) -> None:
        self.replace_gfx24_calibration_function(
            "--capture-tick 300",
            "--capture-tick 120",
        )
        findings = validate(self.root)
        self.assertTrue(any("300-frame warmup" in item for item in findings))

    def test_gfx24_raster_manifest_capture_ticks_must_follow_warmup(self) -> None:
        path = self.root / "Samples" / "RenderVerseSamples" / "Manifests" / "renderverse-samples.json"
        document = json.loads(path.read_text(encoding="utf-8"))
        entry = next(item for item in document["samples"]
                     if item["id"] == "render-quality-lab")
        entry["captureTicks"] = [120, 360, 361]
        path.write_text(json.dumps(document), encoding="utf-8")
        findings = validate(self.root)
        self.assertTrue(any("capture ticks 300/360/361" in item
                            for item in findings))


if __name__ == "__main__":
    unittest.main()
