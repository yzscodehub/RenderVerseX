#!/usr/bin/env python3
"""Focused checks for the optional external asset catalog generator."""

from __future__ import annotations

import json
import subprocess
import sys
import tempfile
from pathlib import Path


REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
GENERATOR = REPOSITORY_ROOT / "Scripts" / "Assets" / "generate_external_asset_catalog_v4.py"


def run(*arguments: str) -> None:
    subprocess.run(
        [sys.executable, str(GENERATOR), *arguments],
        cwd=REPOSITORY_ROOT,
        check=True,
    )


def record_paths(entry: dict) -> list[str]:
    paths = [record["path"] for record in entry["files"]]
    assert paths == sorted(paths)
    assert len(paths) == len(set(paths))
    assert all("\\" not in path and ":" not in path and ".." not in path.split("/") for path in paths)
    return paths


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="rvx-external-catalog-v4-") as temporary:
        first = Path(temporary) / "first.json"
        second = Path(temporary) / "second.json"
        run("--self-test", "--output", str(first))
        run("--self-test", "--output", str(second))
        run("--self-test", "--verify", "--output", str(first))
        assert first.read_bytes() == second.read_bytes(), "generator output is not byte-identical"
        document = json.loads(first.read_text(encoding="utf-8"))

    assert document["schemaId"] == "RVX.SampleAssetCatalog"
    assert document["schemaVersion"] == 4
    entries = {entry["id"]: entry for entry in document["assets"]}
    assert sorted(entries) == [
        "cowboy-town-hall-8k",
        "free-1975-porsche-911-930-turbo",
        "valley-of-desolation-4k",
    ]
    for entry in entries.values():
        assert entry["usagePolicy"] == "optional-nightly"
        assert entry["qualificationEligible"] is False
        assert entry["license"]["coverage"] == "complete-declared-package"
        assert entry["source"]["version"]
        assert entry["attribution"]
        assert entry["modificationNotice"]
        assert entry["assetContentId"]["fileCount"] == len(entry["files"])
        assert entry["assetContentId"]["byteCount"] == sum(record["byteCount"] for record in entry["files"])
        record_paths(entry)

    porsche = entries["free-1975-porsche-911-930-turbo"]
    porsche_paths = record_paths(porsche)
    assert len(porsche_paths) == 30
    assert porsche["assetContentId"]["fileCount"] == 30
    assert porsche["contentIdentity"]["scope"] == "dependency-closure"
    assert porsche["contentIdentity"]["fileCount"] == 29
    assert "models/free_1975_porsche_911_930_turbo/license.txt" in porsche_paths
    assert sum(record["purpose"].startswith("runtime-") for record in porsche["files"]) == 29

    for asset_id in ("cowboy-town-hall-8k", "valley-of-desolation-4k"):
        environment = entries[asset_id]
        assert environment["assetContentId"]["fileCount"] == 2
        assert environment["contentIdentity"]["scope"] == "self-contained-artifact"
        assert environment["contentIdentity"]["fileCount"] == 1
        assert "LICENSE.polyhaven-cc0.txt" in record_paths(environment)

    print("external asset catalog V4 generator tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
