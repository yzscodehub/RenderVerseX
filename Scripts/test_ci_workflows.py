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

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def replace(self, relative: str, old: str, new: str) -> None:
        path = self.root / relative
        text = path.read_text(encoding="utf-8")
        self.assertIn(old, text)
        path.write_text(text.replace(old, new, 1), encoding="utf-8")

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


if __name__ == "__main__":
    unittest.main()
