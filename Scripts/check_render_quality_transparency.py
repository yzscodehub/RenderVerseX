#!/usr/bin/env python3
"""Visibility checks for the GFX26 v4 cooked transparency reference scene.

This adds semantic pixel checks to the existing series/provenance validator.
It does not approve a Golden or replace the temporal and native gates.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
from pathlib import Path

from check_render_quality_bundle import (
    ContractError, _roi_values, _temporal, load_bundle, load_policy, load_sample_contract,
)
from check_render_quality_series import load_series

REGIONS = {
    "alpha": ("BeautyLDR", "alpha-interior"),
    "premultiplied": ("BeautyLDR", "premultiplied-interior"),
    "additive": ("PreToneHDR", "additive-interior"),
    "clear": ("BeautyLDR", "transmission-clear"),
    "rough": ("BeautyLDR", "transmission-rough"),
    "smoke": ("BeautyLDR", "smoke-visible"),
}


def evaluate(samples, smoke_before, smoke_after):
    """Evaluate radiance/color visibility independently from capture loading."""
    failures = []
    metrics = {}
    required = set(REGIONS)
    if set(samples) != required:
        raise ContractError("transparency ROI set is incomplete")
    for name, pixels in samples.items():
        if not pixels or any(
            len(p) != 3 or any(not math.isfinite(v) or v < 0 for v in p)
            for p in pixels
        ):
            raise ContractError(f"{name} ROI is empty, malformed or non-finite")
        mean = [sum(p[c] for p in pixels) / len(pixels) for c in range(3)]
        metrics[name] = {"meanRgb": mean, "peak": max(max(p) for p in pixels)}
    r, g, b = metrics["additive"]["meanRgb"]
    if not (r > 2 * g and g > 2 * b):
        failures.append("additive orange radiance is not distinguishable from backdrop")
    alpha = metrics["alpha"]["meanRgb"]
    if alpha[2] - alpha[0] < 0.10:
        failures.append("alpha blue reference lacks visible color separation")
    premult = samples["premultiplied"]
    white_fraction = sum(min(p) > 0.95 for p in premult) / len(premult)
    metrics["premultiplied"]["nearWhiteFraction"] = white_fraction
    if white_fraction > 0.05:
        failures.append("premultiplied reference is compressed into near-white")
    if metrics["smoke"]["peak"] < 0.08:
        failures.append("smoke is too dark for an unaided visual review")
    for name in ("clear", "rough"):
        values = [p[c] for p in samples[name] for c in range(3)]
        contrast = max(values) - min(values)
        metrics[name]["colorRange"] = contrast
        if contrast < 0.10:
            failures.append(f"{name} transmission lacks a visible striped backdrop")
    if not smoke_before or len(smoke_before) != len(smoke_after):
        raise ContractError("smoke temporal ROIs must have matching nonempty extents")
    if any(len(p) != 3 or any(not math.isfinite(v) for v in p)
           for p in (*smoke_before, *smoke_after)):
        raise ContractError("smoke temporal ROI is malformed or non-finite")
    delta = sum(abs(a - b) for left, right in zip(smoke_before, smoke_after)
                for a, b in zip(left, right)) / (len(smoke_before) * 3)
    metrics["smoke"]["adjacentRgbMae"] = delta
    if delta < 1e-6:
        failures.append("smoke is frozen or its change is not measurable")
    return metrics, failures


def region_pixels(bundle, semantic, region_id, regions):
    if region_id not in regions:
        raise ContractError(f"missing public ROI: {region_id}")
    attachment = bundle.attachments[semantic]
    if (attachment.width, attachment.height) != (1280, 720):
        raise ContractError("GFX26 visibility review requires 1280x720 captures")
    x, y, width, height = regions[region_id]
    left, top = math.floor(x * attachment.width), math.floor(y * attachment.height)
    right = math.ceil((x + width) * attachment.width)
    bottom = math.ceil((y + height) * attachment.height)
    values = _roi_values(attachment, {
        "x": left, "y": top, "width": right - left, "height": bottom - top,
    }, region_id)
    return [values[i:i + 3] for i in range(0, len(values), attachment.channels)]


def review(series_path, manifest, commit, run_id, preset, policy_path):
    series = load_series(series_path, manifest)
    identity = series.qualification_identity
    if (identity["sampleId"] != "transparent-materials"
            or identity["visualContractVersion"] != 4
            or identity["quality"] != "high"
            or identity["view"] != "hero"
            or identity["pipeline"] not in ("forward-plus", "deferred")):
        raise ContractError("visibility review requires transparent-materials v4 High hero")
    if (series.source_commit, series.run_id, series.preset) != (commit, run_id, preset):
        raise ContractError("visibility series source/run/preset mismatch")
    regions, ticks = load_sample_contract(manifest, "transparent-materials")
    if ticks != (120, 360, 361):
        raise ContractError("visibility review requires ticks 120/360/361")
    bundles = {tick.capture_tick: load_bundle(series.path.parent / tick.bundle_path)
               for tick in series.ticks}
    samples = {name: region_pixels(bundles[120], semantic, region, regions)
               for name, (semantic, region) in REGIONS.items()}
    before = region_pixels(bundles[360], "BeautyLDR", "smoke-visible", regions)
    after = region_pixels(bundles[361], "BeautyLDR", "smoke-visible", regions)
    metrics, failures = evaluate(samples, before, after)
    policy = load_policy(policy_path)
    if policy["qualificationIdentity"] != identity:
        raise ContractError("transparency temporal policy identity mismatch")
    temporal, temporal_failures = _temporal(
        [bundles[360], bundles[361]], policy, regions, ticks, commit, run_id, preset)
    failures.extend(temporal_failures)
    return {
        "schema": "RVX.TransparencyShowcaseReview", "schemaVersion": 1,
        "sourceCommit": series.source_commit, "runId": series.run_id,
        "preset": series.preset, "seriesSha256": series.series_sha256,
        "manifestSha256": series.manifest_sha256,
        "qualificationIdentity": identity, "metrics": metrics,
        "temporalMetrics": temporal,
        "policySha256": hashlib.sha256(policy_path.read_bytes()).hexdigest(),
        "failures": failures, "status": "passed" if not failures else "failed",
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--series", type=Path, required=True)
    parser.add_argument("--sample-manifest", type=Path, required=True)
    parser.add_argument("--expected-source-commit", required=True)
    parser.add_argument("--expected-run-id", required=True)
    parser.add_argument("--expected-preset", required=True)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--policy", type=Path, required=True)
    args = parser.parse_args()
    try:
        result = review(args.series, args.sample_manifest, args.expected_source_commit,
                        args.expected_run_id, args.expected_preset, args.policy)
    except (ContractError, OSError, ValueError) as error:
        result = {"schema": "RVX.TransparencyShowcaseReview", "schemaVersion": 1,
                  "status": "failed", "failures": [str(error)]}
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(result, indent=2, allow_nan=False) + "\n",
                           encoding="utf-8")
    print(json.dumps(result, allow_nan=False))
    return 0 if result["status"] == "passed" else 1


if __name__ == "__main__":
    raise SystemExit(main())
