#!/usr/bin/env python3
"""Focused regression tests for pure-ECS legacy path materiality."""
from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from check_pure_ecs_runtime import is_material_legacy_path


class PureEcsRuntimeTests(unittest.TestCase):
    def test_empty_legacy_directory_is_not_a_live_path(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "Spatial/Private/Bounds"
            path.mkdir(parents=True)
            self.assertFalse(is_material_legacy_path(Path(temporary) / "Spatial"))

    def test_legacy_directory_with_a_file_is_live(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "Spatial/Private/Bounds/Bounds.cpp"
            path.parent.mkdir(parents=True)
            path.write_text("// live\n", encoding="utf-8")
            self.assertTrue(is_material_legacy_path(Path(temporary) / "Spatial"))


if __name__ == "__main__":
    unittest.main()
