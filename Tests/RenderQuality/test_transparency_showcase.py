"""Regression cases for the observed GFX26 visibility failures."""
import copy
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "Scripts"))
from check_render_quality_bundle import ContractError
from check_render_quality_transparency import evaluate


class TransparencyVisibilityTests(unittest.TestCase):
    def setUp(self):
        self.samples = {
            "alpha": [(0.20, 0.35, 0.65)] * 20,
            "premultiplied": [(0.65, 0.40, 0.20)] * 20,
            "additive": [(0.90, 0.20, 0.03)] * 20,
            "clear": [(0.20, 0.65, 0.80)] * 20,
            "rough": [(0.30, 0.55, 0.70)] * 20,
            "smoke": [(0.12, 0.13, 0.15)] * 20,
        }
        self.before = [(0.12, 0.13, 0.15)] * 20
        self.after = [(0.121, 0.13, 0.15)] * 20

    def check_failure(self, fragment):
        _, failures = evaluate(self.samples, self.before, self.after)
        self.assertTrue(any(fragment in failure for failure in failures), failures)

    def test_visible_scene_passes(self):
        _, failures = evaluate(self.samples, self.before, self.after)
        self.assertEqual(failures, [])

    def test_observed_gray_additive_is_rejected(self):
        self.samples["additive"] = [(0.193, 0.197, 0.225)] * 20
        self.check_failure("additive orange")

    def test_observed_white_premultiplied_is_rejected(self):
        self.samples["premultiplied"] = [(0.997, 0.994, 0.965)] * 13 + [(0.4, 0.5, 0.3)] * 7
        self.check_failure("near-white")

    def test_observed_dark_smoke_is_rejected_even_when_moving(self):
        self.samples["smoke"] = [(0.008, 0.008, 0.012)] * 20
        self.check_failure("too dark")

    def test_visible_but_frozen_smoke_is_rejected(self):
        self.after = copy.deepcopy(self.before)
        self.check_failure("frozen")

    def test_black_transmission_is_rejected(self):
        for name in ("clear", "rough"):
            with self.subTest(name=name):
                original = self.samples[name]
                self.samples[name] = [(0.0, 0.0, 0.0)] * 20
                self.check_failure(name + " transmission")
                self.samples[name] = original

    def test_missing_roi_fails_closed(self):
        del self.samples["additive"]
        with self.assertRaises(ContractError):
            evaluate(self.samples, self.before, self.after)

    def test_nan_and_empty_roi_fail_closed(self):
        for value in ([], [(float("nan"), 0.5, 0.2)]):
            with self.subTest(value=value):
                self.samples["alpha"] = value
                with self.assertRaises(ContractError):
                    evaluate(self.samples, self.before, self.after)

    def test_mismatched_temporal_extent_fails_closed(self):
        with self.assertRaises(ContractError):
            evaluate(self.samples, self.before, self.after[:-1])


if __name__ == "__main__":
    unittest.main()
