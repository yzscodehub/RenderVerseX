#!/usr/bin/env python3
"""Self-tests for dependency-aware validation planning."""

from __future__ import annotations

import json
import tempfile
import unittest
from pathlib import Path

from architecture_gate_common import GateInputError
from module_policy import Module, Policy, Target
from plan_validation import EVIDENCE_SCHEMA, _read_evidence, build_plan


def module(module_id: str, path: str, dependencies: set[str]) -> Module:
    return Module(
        id=module_id,
        path=Path(path),
        domain="tests" if module_id == "Tests" else "runtime",
        layer="validation" if module_id == "Tests" else "feature",
        public_include_prefixes=(module_id,),
        allowed_modules=frozenset({module_id, *dependencies}),
        shipping_allowed=module_id != "Tests",
        deferred=False,
        owner="test",
    )


class ValidationPlannerTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        root = Path(self.temporary.name).resolve()
        modules = {
            "Core": module("Core", "Source/Runtime/Core", set()),
            "AI": module("AI", "Source/Runtime/AI", {"Core"}),
            "RHI_DX11": module("RHI_DX11", "Source/Runtime/RHI_DX11", {"Core"}),
            "RHI_DX12": module("RHI_DX12", "Source/Runtime/RHI_DX12", {"Core"}),
            "RHI_OpenGL": module("RHI_OpenGL", "Source/Runtime/RHI_OpenGL", {"Core"}),
            "Tests": module(
                "Tests", "Tests", {"AI", "Core", "RHI_DX11", "RHI_DX12", "RHI_OpenGL"}
            ),
        }
        for entry in modules.values():
            (root / entry.path).mkdir(parents=True)
        targets = tuple(
            Target(
                name=f"RVX_{module_id}", aliases=(f"RVX::{module_id}",),
                module=module_id, type="EXECUTABLE" if module_id == "Tests" else "STATIC_LIBRARY",
                source_directory=str(root / entry.path),
                binary_directory=str(root / "build" / module_id), infrastructure=False,
                sources=(f"Private/{module_id}.cpp",), link_libraries=(),
                interface_link_libraries=(), include_directories=(),
                interface_include_directories=(),
            )
            for module_id, entry in modules.items()
        )
        (root / "build").mkdir()
        self.policy = Policy(
            root=root,
            manifest_path=root / "manifest.json",
            inventory_path=root / "inventory.json",
            modules=modules,
            prefix_to_module={key.casefold(): key for key in modules},
            targets=targets,
            target_to_module={
                name: module_id
                for module_id in modules
                for name in (f"RVX_{module_id}", f"RVX::{module_id}")
            },
            disabled_modules=frozenset(),
            source_root=root,
            binary_root=root / "build",
        )
        self.unqualified = {
            "qualifiedCodePullRequests": 0,
            "qualifiedNightlies": 0,
            "plannerMisses": 0,
            "qualified": False,
        }
        self.qualified = {
            "qualifiedCodePullRequests": 10,
            "qualifiedNightlies": 3,
            "plannerMisses": 0,
            "qualified": True,
        }

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def plan(self, files: list[str], mode: str = "observation", qualified: bool = False):
        return build_plan(
            self.policy,
            files,
            base_ref="base",
            head_ref="head",
            mode=mode,
            evidence=self.qualified if qualified else self.unqualified,
        )

    def test_observation_mode_never_reduces_required_matrix(self) -> None:
        plan = self.plan(["Source/Runtime/AI/Private/A.cpp"])
        self.assertTrue(plan["enforcement"]["runFullMatrix"])
        self.assertEqual(plan["enforcement"]["reason"], "observation-mode")
        self.assertIn("shipping", plan["enforcement"]["requiredJobs"])
        self.assertTrue(plan["proposedSelection"]["integrationRequired"])

    def test_reverse_closure_selects_consumers_and_compatibility_backends(self) -> None:
        plan = self.plan(["Source/Runtime/Core/Private/Core.cpp"])
        self.assertEqual(
            plan["affectedModules"],
            ["AI", "Core", "RHI_DX11", "RHI_DX12", "RHI_OpenGL", "Tests"],
        )
        self.assertEqual(plan["proposedSelection"]["compatibilityBackends"], ["DX11", "OpenGL"])
        self.assertEqual(plan["proposedSelection"]["portableUnit"]["Linux"], "full")
        self.assertEqual(
            plan["proposedSelection"]["portableUnit"]["Windows"],
            "platform-sensitive-affected",
        )
        self.assertEqual(
            plan["proposedSelection"]["portableUnit"]["macOS"],
            "platform-sensitive-affected",
        )

    def test_narrow_feature_change_proposes_linux_portable_unit_only(self) -> None:
        plan = self.plan(["Source/Runtime/AI/Private/A.cpp"])
        self.assertEqual(plan["changedModules"], ["AI"])
        self.assertEqual(plan["affectedModules"], ["AI", "Tests"])
        self.assertEqual(plan["proposedSelection"]["compatibilityBackends"], [])
        self.assertEqual(plan["proposedSelection"]["portableUnit"]["Linux"], "full")
        self.assertEqual(plan["proposedSelection"]["portableUnit"]["Windows"], "none")
        self.assertEqual(plan["proposedSelection"]["portableUnit"]["macOS"], "none")

    def test_primary_backend_change_does_not_select_compatibility_backends(self) -> None:
        plan = self.plan(["Source/Runtime/RHI_DX12/Private/DX12Device.cpp"])
        self.assertEqual(plan["changedModules"], ["RHI_DX12"])
        self.assertEqual(plan["proposedSelection"]["compatibilityBackends"], [])
        self.assertEqual(
            plan["proposedSelection"]["portableUnit"]["Windows"],
            "platform-sensitive-affected",
        )

    def test_unknown_and_global_changes_fail_open_to_full_matrix(self) -> None:
        for changed in (["new-root-file.xyz"], ["CMakeLists.txt"], [".github/workflows/ci.yml"]):
            with self.subTest(changed=changed):
                plan = self.plan(changed, mode="selective", qualified=True)
                self.assertTrue(plan["enforcement"]["runFullMatrix"])
                self.assertEqual(plan["enforcement"]["reason"], "unknown-or-global-change")
                self.assertEqual(
                    plan["proposedSelection"]["compatibilityBackends"],
                    ["DX11", "OpenGL"],
                )
                self.assertEqual(plan["proposedSelection"]["portableUnit"]["Windows"], "full")
                self.assertEqual(plan["proposedSelection"]["portableUnit"]["macOS"], "full")

    def test_selective_mode_requires_observation_threshold(self) -> None:
        unqualified = self.plan(["Source/Runtime/AI/Private/A.cpp"], mode="selective")
        qualified = self.plan(
            ["Source/Runtime/AI/Private/A.cpp"], mode="selective", qualified=True
        )
        self.assertTrue(unqualified["enforcement"]["runFullMatrix"])
        self.assertFalse(qualified["enforcement"]["runFullMatrix"])
        self.assertEqual(qualified["enforcement"]["reason"], "dependency-selection-qualified")

    def test_empty_change_set_fails_open(self) -> None:
        plan = self.plan([], mode="selective", qualified=True)
        self.assertTrue(plan["enforcement"]["runFullMatrix"])
        self.assertEqual(plan["enforcement"]["reason"], "empty-change-set")

    def test_invalid_changed_path_is_rejected(self) -> None:
        with self.assertRaises(GateInputError):
            self.plan(["../outside.cpp"])

    def test_evidence_is_strict_and_thresholded(self) -> None:
        evidence_path = self.policy.root / "evidence.json"
        evidence_path.write_text(json.dumps({
            "schema": EVIDENCE_SCHEMA,
            "schemaVersion": 1,
            "qualifiedCodePullRequests": 10,
            "qualifiedNightlies": 3,
            "plannerMisses": 0,
        }), encoding="utf-8")
        self.assertTrue(_read_evidence(evidence_path)["qualified"])
        data = json.loads(evidence_path.read_text(encoding="utf-8"))
        data["plannerMisses"] = 1
        evidence_path.write_text(json.dumps(data), encoding="utf-8")
        self.assertFalse(_read_evidence(evidence_path)["qualified"])
        data["unexpected"] = True
        evidence_path.write_text(json.dumps(data), encoding="utf-8")
        with self.assertRaises(GateInputError):
            _read_evidence(evidence_path)

    def test_plan_hash_is_stable(self) -> None:
        first = self.plan(["Source/Runtime/AI/Private/A.cpp"])
        second = self.plan(["Source/Runtime/AI/Private/A.cpp"])
        self.assertEqual(first["planHash"], second["planHash"])


if __name__ == "__main__":
    unittest.main()
