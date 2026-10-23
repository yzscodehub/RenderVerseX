#!/usr/bin/env python3
"""Fail-closed fixtures for the renderer feature ownership gate."""

from __future__ import annotations

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
CHECKER = REPO_ROOT / "Scripts/check_renderer_feature_ownership.py"
FEATURE_ROOT = "Source/Runtime/Render/Private/Features"
SCENE_RENDERER = "Source/Runtime/Render/Include/Render/Renderer/SceneRenderer.h"
COMPOSITION = "Source/Runtime/Render/Private/Features/DefaultForwardComposition.cpp"


def write_fixture(
    root: Path,
    *,
    adapter_header: str = "class CleanFeatureAdapter { int m_state = 0; };\n",
    adapter_source: str = "void CleanFeatureAdapter::Prepare() {}\n",
    scene_renderer: str = "class SceneRenderer { int m_frame = 0; };\n",
    composition: str = (
        "void Compose() { auto bundle = CreateDefaultRenderPassBundle(); (void)bundle; }\n"
    ),
) -> None:
    (root / "CMakeLists.txt").write_text("cmake_minimum_required(VERSION 3.25)\n")
    feature_root = root / FEATURE_ROOT
    feature_root.mkdir(parents=True)
    # Both production adapter roots are required by the checker.  The public
    # root intentionally contains no adapter in this fixture.
    (root / "Source/Runtime/Render/Include/Render/Features").mkdir(parents=True)
    (feature_root / "CleanFeatureAdapter.h").write_text(adapter_header, encoding="utf-8")
    (feature_root / "CleanFeatureAdapter.cpp").write_text(adapter_source, encoding="utf-8")
    scene_path = root / SCENE_RENDERER
    scene_path.parent.mkdir(parents=True)
    scene_path.write_text(scene_renderer, encoding="utf-8")
    (root / COMPOSITION).write_text(composition, encoding="utf-8")


def run_checker(root: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, str(CHECKER), "--root", str(root)],
        cwd=REPO_ROOT,
        text=True,
        capture_output=True,
        check=False,
    )


class RendererFeatureOwnershipTests(unittest.TestCase):
    def test_compliant_composition_fixture_passes(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            write_fixture(root)
            result = run_checker(root)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_feature_adapter_registry_member_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            write_fixture(
                root,
                adapter_header=(
                    "class RegistryFeatureAdapter { RenderPassRegistry* m_registry = nullptr; };\n"
                ),
            )
            result = run_checker(root)
            self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
            self.assertIn("forbidden pass ownership", result.stdout)

    def test_feature_adapter_implementation_member_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            write_fixture(
                root,
                adapter_source=(
                    "class HiddenFeatureAdapter { IRenderPass* m_hidden = nullptr; };\n"
                ),
            )
            result = run_checker(root)
            self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
            self.assertIn("forbidden pass ownership", result.stdout)

    def test_feature_adapter_pass_constructor_injection_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            write_fixture(
                root,
                adapter_header=(
                    "class DeferredFeatureAdapter {\n"
                    "public:\n"
                    "    DeferredFeatureAdapter(const OpaquePass& pass);\n"
                    "};\n"
                ),
            )
            result = run_checker(root)
            self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
            self.assertIn("constructor injects a pass implementation", result.stdout)

    def test_scene_renderer_adapter_and_pass_pointer_caches_fail_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            write_fixture(
                root,
                scene_renderer=(
                    "class SceneRenderer {\n"
                    "    DeferredFeatureAdapter* m_deferred = nullptr;\n"
                    "    OpaquePass* m_opaque = nullptr;\n"
                    "};\n"
                ),
            )
            result = run_checker(root)
            self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
            self.assertIn("caches built-in feature adapter", result.stdout)
            self.assertIn("caches concrete pass pointer", result.stdout)

    def test_central_direct_pass_construction_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            write_fixture(
                root,
                composition=(
                    "void Compose() {\n"
                    "    auto pass = std::make_unique<OpaquePass>();\n"
                    "    auto bundle = CreateDefaultRenderPassBundle();\n"
                    "}\n"
                ),
            )
            result = run_checker(root)
            self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
            self.assertIn("must not construct a concrete render pass directly", result.stdout)

    def test_missing_or_multiple_bundle_factory_calls_fail_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            write_fixture(root, composition="void Compose() {}\n")
            result = run_checker(root)
            self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
            self.assertIn("exactly once, found 0", result.stdout)

        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            write_fixture(
                root,
                composition=(
                    "void Compose() { CreateDefaultRenderPassBundle(); "
                    "CreateDefaultRenderPassBundle(); }\n"
                ),
            )
            result = run_checker(root)
            self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
            self.assertIn("exactly once, found 2", result.stdout)

    def test_missing_required_source_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            write_fixture(root)
            (root / COMPOSITION).unlink()
            result = run_checker(root)
            self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
            self.assertIn("required DefaultForwardComposition source is missing", result.stdout)


if __name__ == "__main__":
    unittest.main()
